// What the game's plaque shows of a track: artist, title and album, read from the file or its name, in letters its font draws.
import type { Lines } from './app/radio-link';

const PLAQUE_CHARS = 40;                 // what the game's plaque shows of a line

/** Artist and title from the file name ("Artist - Title.mp3"), else the name alone. */
export function nameTags(name: string): Lines {
  const stem = name.replace(/\.[^.]+$/, ''), at = stem.indexOf(' - ');
  return at > 0 ? { artist: stem.slice(0, at).trim(), title: stem.slice(at + 3).trim(), album: '' } : { artist: '', title: stem, album: '' };
}

// The plaque's font draws ASCII for certain; anything else is written with the nearest ASCII letters.
const CYRILLIC: Record<string, string> = {
  а: 'a', б: 'b', в: 'v', г: 'g', д: 'd', е: 'e', ё: 'yo', ж: 'zh', з: 'z', и: 'i', й: 'y', к: 'k', л: 'l', м: 'm', н: 'n', о: 'o', п: 'p', р: 'r', с: 's', т: 't',
  у: 'u', ф: 'f', х: 'kh', ц: 'ts', ч: 'ch', ш: 'sh', щ: 'shch', ъ: '', ы: 'y', ь: '', э: 'e', ю: 'yu', я: 'ya',
};
const NEAREST: Record<string, string> = { ł: 'l', đ: 'd', ø: 'o', æ: 'ae', œ: 'oe', ß: 'ss', ı: 'i', '–': '-', '—': '-', '‘': "'", '’': "'", '“': '"', '”': '"', '…': '...' };
const cased = (from: string, to: string) => from === from.toLowerCase() ? to : to.charAt(0).toUpperCase() + to.slice(1);

/** The text in ASCII: accents dropped, Cyrillic romanized, a sign with no nearest letter is "?". */
export function gameText(text: string): string {
  let out = '';
  for (const ch of text.normalize('NFC')) {
    const low = ch.toLowerCase(), base = ch.normalize('NFD').replace(/\p{M}/gu, '');
    if (ch.charCodeAt(0) < 0x80) out += ch.charCodeAt(0) >= 0x20 ? ch : ' ';
    else if (CYRILLIC[low] !== undefined) out += cased(ch, CYRILLIC[low]);
    else if (NEAREST[low] !== undefined) out += cased(ch, NEAREST[low]);
    else out += base && base.charCodeAt(0) < 0x80 ? base : '?';
  }
  return out;
}

/** What the game's plaque can show: no artist in the title, a line no longer than its own longest ("&" is drawn as it is). */
export function plaqueTags(tags: Lines, album = 'My music'): Lines {
  const fit = (s: string) => { const t = gameText(s).replace(/\s+/g, ' ').trim(); return t.length > PLAQUE_CHARS ? t.slice(0, PLAQUE_CHARS - 3).trimEnd() + '...' : t; };
  const own = tags.artist + ' - ';
  const title = tags.artist && tags.title.toLowerCase().startsWith(own.toLowerCase()) ? tags.title.slice(own.length) : tags.title;
  return { artist: fit(tags.artist) || 'Unknown artist', title: fit(title), album: fit(tags.album) || fit(album) || 'My music' };
}

const synchsafe = (b: Uint8Array, at: number) => ((b[at] & 0x7f) << 21) | ((b[at + 1] & 0x7f) << 14) | ((b[at + 2] & 0x7f) << 7) | (b[at + 3] & 0x7f);

// Encoding 1 is UTF-16 with a BOM that says the byte order (none: big-endian, as ID3 says); the label "utf-16" would always be little-endian.
function id3Text(b: Uint8Array): string {
  const enc = b[0] === 1 ? (b[1] === 0xff && b[2] === 0xfe ? 'utf-16le' : 'utf-16be') : ['latin1', '', 'utf-16be', 'utf-8'][b[0]] || 'latin1';
  return new TextDecoder(enc).decode(b.subarray(1)).replace(/\0[\s\S]*$/, '').replace(/^\uFEFF+/, '').trim();   // an encoder may write the BOM twice
}

/** Artist, title and album: the ID3v2 tags at the start of an mp3, else what the file name says.  Reads one file, its first megabyte at most. */
export async function tagsOf(handle: FileSystemFileHandle): Promise<Lines> {
  const out = nameTags(handle.name);
  try {
    const b = new Uint8Array(await (await handle.getFile()).slice(0, 1 << 20).arrayBuffer());
    if (String.fromCharCode(b[0], b[1], b[2]) !== 'ID3' || b[3] < 3) return out;   // ID3v2.3 or 2.4 only
    const end = Math.min(10 + synchsafe(b, 6), b.length);
    for (let p = 10; p + 10 <= end;) {
      const id = String.fromCharCode(b[p], b[p + 1], b[p + 2], b[p + 3]);
      const size = b[3] === 4 ? synchsafe(b, p + 4) : new DataView(b.buffer).getUint32(p + 4);
      if (!size || p + 10 + size > end) break;
      const text = id3Text(b.subarray(p + 10, p + 10 + size));
      if (id === 'TIT2') out.title = text || out.title;
      if (id === 'TPE1') out.artist = text || out.artist;
      if (id === 'TALB') out.album = text || out.album;
      p += 10 + size;
    }
  } catch { /* a file that cannot be read for its tags keeps its name */ }
  return out;
}
