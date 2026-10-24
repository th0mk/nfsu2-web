// The music folder: a directory the player picks with the browser's directory picker (Chrome, Edge) or with a file input that takes
// a folder (Firefox, Safari), and the audio files in it and in its subfolders, by name only (a file is opened when it is played).
const AUDIO = /\.(mp3|ogg|oga|opus|wav|flac|m4a)$/i;
const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });
const MAX_DEPTH = 8, MAX_FOLDERS = 2000;   // a folder tree deeper or wider than this is not a music library

export type Folder = FileSystemDirectoryHandle & {
  queryPermission(o: { mode: 'read' }): Promise<PermissionState>;
  requestPermission(o: { mode: 'read' }): Promise<PermissionState>;
};
type Dir = FileSystemDirectoryHandle & { values(): AsyncIterable<FileSystemHandle> };
type Found = { files: FileSystemFileHandle[]; paths: string[] };
/** files[i] is at paths[i] relative to the folder ("a.mp3", "Album/a.mp3"); handle: the folder itself, when the browser gives one to keep. */
export type Picked = Found & { name: string; handle?: Folder };

const dirOf = (path: string) => path.slice(0, Math.max(0, path.lastIndexOf('/')));
/** The name of the subfolder a track is in ('' for the folder itself). */
export const subfolderOf = (path: string) => dirOf(path).split('/').pop()!;
const byPath = (a: string, b: string) => {
  const da = dirOf(a), db = dirOf(b);
  return da === db ? collator.compare(a, b) : !da ? -1 : !db ? 1 : collator.compare(da, db);
};

/** The files in the order of the playlist: the folder's own first, then each subfolder by name, the files of each by name. */
export function sortedTracks(files: readonly FileSystemFileHandle[], paths?: readonly string[]): Found {
  const keys = files.map((f, i) => paths?.[i] ?? f.name), order = keys.map((_, i) => i).sort((a, b) => byPath(keys[a], keys[b]));
  return { files: order.map(i => files[i]), paths: order.map(i => keys[i]) };
}

/** The audio files of the folder and of its subfolders; a folder that is its own ancestor or deeper than MAX_DEPTH is not entered,
 *  a subfolder that cannot be listed is left out. */
async function filesIn(root: FileSystemDirectoryHandle): Promise<Found> {
  const out: Found = { files: [], paths: [] };
  let folders = 0;
  const walk = async (d: Dir, prefix: string, above: Dir[]): Promise<void> => {
    for await (const e of d.values()) {
      if (e.kind === 'file' && AUDIO.test(e.name)) {
        out.files.push(e as FileSystemFileHandle);
        out.paths.push(prefix + e.name);
      } else if (e.kind === 'directory' && above.length < MAX_DEPTH && ++folders <= MAX_FOLDERS) {
        const chain = [...above, d], same = await Promise.all(chain.map(a => a.isSameEntry?.(e as Dir))).catch(() => []);
        if (!same.some(Boolean)) await walk(e as Dir, `${prefix}${e.name}/`, chain).catch(() => {});
      }
    }
  };
  await walk(root as Dir, '', []);
  return out;
}

/** A folder can be picked here: by the directory picker, or by an input with webkitdirectory, which Safari on an iPhone or iPad lists no folders for. */
export const canPickMusic = () => 'showDirectoryPicker' in globalThis || !(/iPhone|iPad|iPod/.test(navigator.userAgent) || /Macintosh/.test(navigator.userAgent) && navigator.maxTouchPoints > 1);

/** The folder the player picks, or null when the player cancels or the browser cannot pick a folder.  Any other failure is thrown. */
export async function pickMusic(): Promise<Picked | null> {
  const pick = (globalThis as { showDirectoryPicker?: (o: { mode: 'read' }) => Promise<FileSystemDirectoryHandle> }).showDirectoryPicker;
  if (!pick) return pickFromInput();
  try {
    const dir = await pick.call(globalThis, { mode: 'read' });
    return { ...await filesIn(dir), name: dir.name, handle: dir as Folder };
  } catch (e) {
    if (e instanceof DOMException && e.name === 'AbortError') return null;
    throw e;
  }
}

/** For a browser without a directory picker: a file input that takes a folder (webkitRelativePath "Folder/Sub/a.mp3").
 *  It cannot be asked again after a reload, so the folder is not kept. */
function pickFromInput(): Promise<Picked | null> {
  if (typeof document === 'undefined') return Promise.resolve(null);
  return new Promise(resolve => {
    const input = Object.assign(document.createElement('input'), { type: 'file', webkitdirectory: true, multiple: true });
    input.addEventListener('cancel', () => resolve(null));
    input.addEventListener('change', () => {
      const list = Array.from(input.files ?? []), picked: Picked = { files: [], paths: [], name: '' };
      for (const f of list) {
        const parts = (f.webkitRelativePath || f.name).split('/');
        picked.name ||= parts.length > 1 ? parts[0] : '';
        if (!AUDIO.test(f.name)) continue;
        picked.files.push({ kind: 'file', name: f.name, getFile: async () => f } as unknown as FileSystemFileHandle);
        picked.paths.push(parts.length > 1 ? parts.slice(1).join('/') : f.name);
      }
      resolve(list.length ? picked : null);
    });
    input.click();
  });
}

/** One request to the folder store (IndexedDB "nfsu2.radio"); any failure, or a store that is not there: undefined.  The connection is closed after it. */
function inStore<T>(mode: IDBTransactionMode, run: (s: IDBObjectStore) => IDBRequest<T>): Promise<T | undefined> {
  return new Promise(resolve => {
    let db: IDBDatabase | undefined;
    const done = (v?: T) => { try { db?.close(); } catch { /* already closed */ } resolve(v); };
    try {
      const open = indexedDB.open('nfsu2.radio', 1);
      open.onupgradeneeded = () => open.result.createObjectStore('folder');
      open.onerror = open.onblocked = () => done();
      open.onsuccess = () => {
        try {
          db = open.result;
          const tx = db.transaction('folder', mode), r = run(tx.objectStore('folder'));
          tx.oncomplete = () => done(r.result);
          tx.onerror = tx.onabort = () => done();
        } catch { done(); }
      };
    } catch { done(); }
  });
}

/** The folder handle kept from the last visit: the only way to open the folder again after a reload. */
export const keptFolder = () => inStore<Folder>('readonly', s => s.get('folder'));
export const keepFolder = (handle: Folder) => inStore('readwrite', s => s.put(handle, 'folder'));

/** The folder's files when the browser allows it (ask: it may show its question, which needs a click), else null. */
export async function reopenFolder(handle: Folder, ask: boolean): Promise<Found | null> {
  const state = await handle[ask ? 'requestPermission' : 'queryPermission']({ mode: 'read' });
  return state === 'granted' ? filesIn(handle) : null;
}
