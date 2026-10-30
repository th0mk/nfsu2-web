// A profile Underground 2 saved ("NFS Underground 2/<NAME>/<NAME>", 54,966 bytes; the page takes it through
// '@game/profile', src/games/ug2/profile.ts, and Most Wanted's from src/games/mw/profile.ts)
// as far as the page reads or changes it: the file's checksums and the game's
// own Save option (Options, Gameplay, Save: Autosave, Prompt or Off), its
// name (renameProfile), and, for one from the PC game, its checks, the cars
// a mod added and a save editor's stale checksums (profileName, modCars, sealed).
//
// Layout and checksum: the clean-room description of this SPEED2.EXE (v1.2 NA,
// the README's SHA-256) in github.com/whoismept/OpenUG2,
// docs/cleanroom-settings/SPEC.md §3.2-3.5 (file), §5.1 and §5.8 (Save):
//   0   "20CM"          4  u32 file size     8  u32 sizeA = 8     12 u32 sizeB = 54,930
//   16  checksum(A)     20 checksum(B)       24 checksum(bytes 0..23)
//   28  block A: u32 version 0x10D, u32 payload size 54,930
//   36  block B, the payload: +50,987 u8 autosave, +50,988 u8 prompt, +53,761 the name (32 bytes, NUL-padded)
//   0xA16A (in the file) u32 the bank, as the save editors have it (app-test.mjs pcsave checks it in the game)
// autosave 1 shows "Autosave" (prompt is then 1 too: selecting Autosave sets
// both), autosave 0 with prompt 1 "Prompt", both 0 "Off".  A new profile is on
// Prompt (the tests' "save the profile?" after a purchase, after Options), whose
// question opens on "No".  checksum(D) is CRC-32/BZIP2 of D but its last 4
// bytes, XOR those 4 bytes read big-endian; stored little-endian.  The name's
// place agrees with the save editors that work on players' real profiles
// (0xD225 in hackch/NFSU2-Savegame-Editor); those editors leave the checksums
// stale.  The game loads such a file all the same (checked 2026-10-22: a
// money change with B's and the header's checksums stale, written straight
// into the save folder, loaded with the changed bank).  One from elsewhere is
// taken as edited (profileName) and comes in sealed again, with a warning, so
// the page's own checks below take it too; one in the save folder whose
// checksums do not hold is left alone instead of being "repaired".  Checked in
// the game (2026-10-19): a profile it created came in on Prompt, and after
// withAutosave it loaded with Options, Gameplay, Save at Autosave and every
// other Gameplay option as before.  Every check below must still hold before
// the page changes a file in the save folder.
//
// Pure (no DOM, no OPFS) so it can be checked under node; turnOnAutosave and
// renameProfile take the save folder's file functions.

import { fill, N, t, type Vars } from './i18n';
import { checksum, isMemcardFile } from './memcard';

export const PROFILE_SIZE = 54966;
const PAYLOAD = 54930, VERSION = 0x10D;
const B = 36;                                        // the payload's start in the file
const AUTOSAVE = B + 50987, PROMPT = B + 50988, NAME = B + 53761, MONEY = 0xA16A;

/** The game's checksum of a block (SPEC §3.4, memcard.ts): 0 below 4 bytes. */
export { checksum };

export type SaveMode = 'autosave' | 'prompt' | 'off';

const u32 = (f: Uint8Array, at: number) => new DataView(f.buffer, f.byteOffset, f.byteLength).getUint32(at, true);
const lowerAscii = (s: string) => s.replace(/[A-Z]/g, c => String.fromCharCode(c.charCodeAt(0) + 32));

/**
 * The Save option of a profile file, or why the page does not trust the file
 * enough to change it: its size, header, checksums, version, the name inside
 * against its folder's (ASCII letters in any case), and both Save bytes 0 or 1.
 */
export function readSaveMode(file: Uint8Array, name: string): { mode: SaveMode } | { bad: string } {
  const bad = checkProfile(file, name);
  if (bad) return { bad: fill(bad.t, bad.v) };
  const a = file[AUTOSAVE], p = file[PROMPT];
  if (a > 1 || p > 1) return { bad: `unexpected Save bytes ${a}, ${p}` };
  return { mode: a ? 'autosave' : p ? 'prompt' : 'off' };
}

/** why a check refuses a file, as written (English with {name}s for v): filled for the log, translated for the page */
interface Why { t: string; v?: Vars }

/** Why a file is not a profile the game saved under this name (readSaveMode's checks but the Save bytes), or null. */
function checkProfile(file: Uint8Array, name: string): Why | null {
  const bad = checkFile(file);
  if (bad) return bad;
  if (lowerAscii(folderName(file)) !== lowerAscii(name)) return { t: N('the name inside is "{name}"'), v: { name: nameInside(file) } };
  return null;
}

/** checkProfile's checks of the file itself: SPEC §3.5's (the game loads one whose checksums are stale, though). */
function checkFile(file: Uint8Array): Why | null {
  return checkShape(file) ?? (sealedRight(file) ? null : { t: N('its checksums do not match (edited outside the game?)') });
}

/** checkFile's checks but the checksums: the container, its block sizes and the version. */
function checkShape(file: Uint8Array): Why | null {
  if (!isProfileFile(file)) return { t: N('not a profile file (no "20CM")') };
  if (file.length !== PROFILE_SIZE) return { t: N('{n} bytes, not {size}'), v: { n: file.length, size: PROFILE_SIZE } };
  if (u32(file, 4) !== file.length || u32(file, 8) !== 8 || u32(file, 12) !== PAYLOAD) return { t: N('unexpected block sizes') };
  if (u32(file, 28) !== VERSION || u32(file, 32) !== PAYLOAD) return { t: N('saved by another version of the game (version {got}, not {want})'), v: { got: u32(file, 28), want: VERSION } };
  return null;
}

const sealedRight = (file: Uint8Array) => u32(file, 16) === checksum(file.subarray(28, B)) &&
  u32(file, 20) === checksum(file.subarray(B)) && u32(file, 24) === checksum(file.subarray(0, 24));

function nameInside(file: Uint8Array): string {
  let inside = '';
  for (let i = NAME; i < NAME + 32 && file[i]; i++) inside += String.fromCharCode(file[i]);
  return inside;
}

/**
 * The name of the folder the game saves a profile in: the name inside, at
 * most 31 bytes of it (0x6FCAB8 copies no more), as the I/O worker reads a
 * path (UTF-8, U+FFFD for bytes that are not).  For a name the game's field
 * makes, the name inside itself.  A cover shoot whose save failed could name
 * a profile with stale heap bytes (runtime/overrides.c sub_004e8b70); Rename
 * gives such a profile a name of letters and digits again.
 */
function folderName(file: Uint8Array): string {
  let n = 0;
  while (n < 31 && file[NAME + n]) n++;
  return new TextDecoder().decode(file.subarray(NAME, NAME + n));
}

/** A file that starts as the game's save files do ("20CM"): a profile, or one of another version. */
export const isProfileFile = isMemcardFile;

/** A profile's bank; null: not a profile of this version. */
export const profileMoney = (file: Uint8Array): number | null => file.length === PROFILE_SIZE && isProfileFile(file) ? u32(file, MONEY) : null;

/**
 * The name inside a profile file that comes from elsewhere (the PC game's
 * NFS Underground 2 folder, a backup), or why the page does not take it:
 * checkProfile's checks but the folder's name and the checksums, and a name
 * the game's own field makes (isProfileName).  edited: its checksums do not
 * hold, as a save editor leaves them; it comes in as sealed() makes it, so
 * Autosave and Rename take it.  A save editor that makes them again is not seen here.
 */
export function profileName(file: Uint8Array): { name: string; edited: boolean } | { bad: string } {
  const bad = checkShape(file);
  if (bad) return { bad: t(bad.t, bad.v) };
  const name = nameInside(file);
  if (!isProfileName(name)) return { bad: t('the name inside, "{name}", is not one the game makes (1 to {n} letters and digits)', { name, n: NAME_MAX }) };
  return { name, edited: !sealedRight(file) };
}

// ------------------------------------------------------- mod cars --
//
// The cars come first in the payload (FEDatabase+0x3c0, written by 0x5166b0
// and read back as they are by 0x516850):
//   0      48 stock cars, 24 bytes each: +20 u32 the car type's index (0xFFFFFFFF unused)
//   1152   u32 stock cars in use (31)
//   1156   20 customized cars, 1,072 bytes each
//   22596  5 career cars, 2,034 bytes each
//   32770  6 preset cars, 1,076 bytes each (after a u32 at 32766)
// The customized, career and preset cars hold their type as a name hash at
// +28 (0: unused), then 170 part hashes.  The hash is bStringHash (0x43db50)
// of the type's name, as CarTypeInfo +0xd0 holds it.  0x503880 reads an
// unknown one's CarTypeInfo through a NULL pointer (0x610130 returns 0).
// Unknown part hashes find no part (0x61b980), so that slot is empty; the page
// does not check those (the game's part table is 12,000 entries).  Checked
// against players' profiles: their cars are all of the 31 types below.

/** The player cars, CarTypeInfo 0..30 in GLOBAL/GLOBALB.BUN (chunk 0x34600); 31..45 are traffic. */
const PLAYER_CARS = ['PEUGOT', 'FOCUS', 'COROLLA', '240SX', 'MIATA', 'CIVIC', 'PEUGOT106', 'CORSA', 'HUMMER', 'NAVIGATOR',
  'ESCALADE', 'TIBURON', 'SENTRA', 'CELICA', 'IS300', 'SUPRA', 'GOLF', 'A3', 'RSX', 'ECLIPSE', 'TT', 'RX8', '350Z', 'G35',
  '3000GT', 'GTO', 'MUSTANGGT', 'SKYLINE', 'LANCEREVO8', 'RX7', 'IMPREZAWRX'];
export const bStringHash = (s: string) => { let h = 0xFFFFFFFF; for (let i = 0; i < s.length; i++) h = (Math.imul(h, 33) + s.charCodeAt(i)) >>> 0; return h; };
const PLAYER_CAR_HASHES = new Set(PLAYER_CARS.map(bStringHash));

/** How many cars in a profile (one profileName took) are of a type the game's player cars do not have: cars a mod added. */
export function modCars(file: Uint8Array): number {
  const at = (o: number) => u32(file, B + o);
  let n = 0;
  for (let i = 0; i < 48; i++) { const t = at(i * 24 + 20); if (t !== 0xFFFFFFFF && t >= PLAYER_CARS.length) n++; }
  const typed = (o: number) => { const h = at(o + 28); if (h && !PLAYER_CAR_HASHES.has(h)) n++; };
  for (let i = 0; i < 20; i++) typed(1156 + i * 1072);
  for (let i = 0; i < 5; i++) typed(22596 + i * 2034);
  for (let i = 0; i < 6; i++) typed(32770 + i * 1076);
  return n;
}

/** The file's checksums made again (A's, B's, then the header's, which covers the other two). */
function seal(f: Uint8Array): Uint8Array {
  const dv = new DataView(f.buffer, f.byteOffset, f.byteLength);
  dv.setUint32(16, checksum(f.subarray(28, B)), true);
  dv.setUint32(20, checksum(f.subarray(B)), true);
  dv.setUint32(24, checksum(f.subarray(0, 24)), true);
  return f;
}

/** A copy of a profile file profileName took as edited, with its checksums made again: as if the game had saved it. */
export const sealed = (file: Uint8Array) => seal(file.slice());

/** A copy of a profile file readSaveMode accepted, with Save at Autosave (both bytes 1, as the game's menu sets it) and its checksums made again. */
export function withAutosave(file: Uint8Array): Uint8Array {
  const f = file.slice();
  f[AUTOSAVE] = 1; f[PROMPT] = 1;
  return seal(f);
}

/** A copy of a profile file the checks accepted, with another name inside (NUL-padded, as the game writes it) and its checksums made again. */
export function withName(file: Uint8Array, name: string): Uint8Array {
  const f = file.slice();
  f.fill(0, NAME, NAME + 32);
  for (let i = 0; i < name.length; i++) f[NAME + i] = name.charCodeAt(i);
  return seal(f);
}

// ------------------------------------------------- once per profile --

/** The save folder's file of the profiles turnOnAutosave has looked at: each one gets Autosave once. */
export const AUTOSAVE_SEEN = 'autosave-profiles.txt';
const SEEN_HEAD = `# Profiles the page has looked at for settings.ini's "autosave": each one is set
# to Autosave once (if it was on Prompt, the game's default), then left as the
# game's Options, Gameplay, Save set it.  Delete a name to have it set again.
`;

export interface SaveFiles {
  listFiles(): Promise<{ path: string }[]>;
  readBytes(path: string): Promise<Uint8Array>;
  readText(path: string): Promise<string | null>;
  writeFile(path: string, data: string | Uint8Array): Promise<void>;
}

/**
 * Before a Play (no game runs in this tab): every profile not looked at yet that
 * is on Prompt is set to Autosave.  Profiles on Autosave or Off, and those set,
 * are remembered (AUTOSAVE_SEEN), so a choice made in the game later stays; a
 * file readSaveMode does not trust is left alone and looked at again next time.
 * Names of profiles that are gone are dropped (a new profile of the same name
 * gets Autosave too).  turnedOn: the profiles set now; notes: one line each for the log.
 */
export async function turnOnAutosave(fs: SaveFiles, profilesDir: string): Promise<{ turnedOn: string[]; notes: string[] }> {
  const seenText = await fs.readText(AUTOSAVE_SEEN) ?? '';
  const seen = new Set(seenText.split('\n').map(l => l.trim()).filter(l => l && !l.startsWith('#')));
  const turnedOn: string[] = [], notes: string[] = [], now: string[] = [];
  for (const { path } of await fs.listFiles()) {
    const p = path.split('/');
    if (p.length !== 3 || p[0] !== profilesDir || lowerAscii(p[2]) !== lowerAscii(p[1])) continue;
    const name = p[1];
    if (seen.has(name)) { now.push(name); continue; }
    const file = await fs.readBytes(path);
    const r = readSaveMode(file, name);
    if ('bad' in r) { notes.push(`${name} left as it is: ${r.bad}`); continue; }
    if (r.mode === 'prompt') {
      await fs.writeFile(path, withAutosave(file));
      turnedOn.push(name);
      notes.push(`${name}: Save set from Prompt to Autosave (Options, Gameplay, Save in the game)`);
    } else notes.push(`${name}: Save is ${r.mode === 'off' ? 'Off' : 'Autosave'} already, left as it is`);
    now.push(name);
  }
  const text = SEEN_HEAD + now.map(n => n + '\n').join('');
  if (text !== seenText && (now.length || seenText)) await fs.writeFile(AUTOSAVE_SEEN, text);
  return { turnedOn, notes };
}

// ------------------------------------------------------------- rename --

/**
 * A name the game's own name field takes: letters and digits, up to 7 of them
 * (tests/scripts/kb_profile.txt: the field at 0x4ff310 keeps nothing else).
 */
export const NAME_MAX = 7;
export const isProfileName = (s: string) => /^[A-Za-z0-9]+$/.test(s) && s.length <= NAME_MAX;

/**
 * A cover shoot's save, a folder of its own beside its profile's: "<NAME> DVD 1",
 * "<NAME> Magazine 2" (the game puts "DVD %d" or "Magazine %d", 0x79acc0 and
 * 0x79acb4, after the profile's name).  Its file (1,048,612 bytes) does not
 * hold the name, so the folder's name is all that ties it to the profile.
 * A profile's name has no spaces, so no profile looks like one.
 */
export function coverOf(folder: string): { profile: string; cover: string } | null {
  const m = /^(.+) ((?:DVD|Magazine) [0-9]+)$/.exec(folder);
  return m ? { profile: m[1], cover: m[2] } : null;
}

export interface RenameFiles extends SaveFiles {
  /** a file, or a folder with everything in it */
  remove(path: string): Promise<void>;
}

/**
 * Rename a profile as if the game had made it under the new name: its folder and
 * file, the name inside (checksums made again), its cover shoots' folders, and
 * its line in AUTOSAVE_SEEN (so a Save choice made in the game stays).  Refused,
 * with nothing written, when the new name is not one the game's field takes,
 * when another save has it (the I/O worker finds names in any case), or when the
 * file fails readSaveMode's checks.  A change of case only is a rename too.  The
 * new folders are written in full before the old ones go: a rename that stops
 * halfway leaves the profile as it was, under its old name.
 */
export async function renameProfile(fs: RenameFiles, profilesDir: string, from: string, to: string):
    Promise<{ files: number } | { refused: string }> {
  if (!isProfileName(to)) return { refused: t('A profile\'s name is 1 to {n} letters and digits, as in the game.', { n: NAME_MAX }) };
  if (to === from) return { files: 0 };
  const folders = new Map<string, string[]>();      // each folder in profilesDir: its files
  for (const { path } of await fs.listFiles()) {
    const p = path.split('/');
    if (p.length >= 3 && p[0] === profilesDir) folders.set(p[1], [...folders.get(p[1]) ?? [], path]);
  }
  const same = (a: string, b: string) => lowerAscii(a) === lowerAscii(b);
  const own = (folder: string, path: string) => { const p = path.split('/'); return p.length === 3 && same(p[2], folder); };
  const main = folders.get(from)?.find(path => own(from, path));
  if (!main) return { refused: t('There is no profile {name} in this browser.', { name: from }) };
  const file = await fs.readBytes(main);
  const bad = checkProfile(file, from);
  if (bad) return { refused: t('{name} cannot be renamed here: {why}.', { name: from, why: t(bad.t, bad.v) }) };
  const moves: [string, string][] = [[from, to]];
  for (const f of folders.keys()) { const c = coverOf(f); if (c && same(c.profile, from)) moves.push([f, `${to} ${c.cover}`]); }
  const moving = new Set(moves.map(m => m[0]));
  for (const f of folders.keys()) {
    if (moving.has(f)) continue;
    if (same(f, to)) return { refused: t('There is a profile {name} already.', { name: f }) };
    if (same(coverOf(f)?.profile ?? '', to)) return { refused: t('There is a cover shoot {name} of that name already.', { name: f }) };
  }
  let files = 0;
  try {
    for (const [old, now] of moves) for (const path of folders.get(old)!) {
      const rest = path.slice(profilesDir.length + old.length + 2);
      await fs.writeFile(`${profilesDir}/${now}/${own(old, path) ? now : rest}`, path === main ? withName(file, to) : await fs.readBytes(path));
      files++;
    }
  } catch (e) {
    for (const [, now] of moves) await fs.remove(`${profilesDir}/${now}`).catch(() => {});
    throw e;
  }
  const seen = await fs.readText(AUTOSAVE_SEEN);
  const renamed = seen?.split('\n').map(l => l.trim() === from ? to : l).join('\n');
  if (seen !== null && renamed !== seen) await fs.writeFile(AUTOSAVE_SEEN, renamed!);
  for (const [old] of moves) await fs.remove(`${profilesDir}/${old}`);
  return { files };
}
