// Most Wanted's alias saves ("NFS Most Wanted/<ALIAS>/<ALIAS>", 63,596 bytes) as far as the page reads or changes
// them, behind '@game/profile' (web/src/games/profile.ts SaveFormat; Underground 2's is src/app/profile.ts): their
// checks, the name inside (profileName, renameProfile), the bank (profileMoney), and an edited one's checks made again
// (sealed).  Layout: docs/mw/save-format.md, the clean-room spec (PLAN D-21, TRACKER M6-22), offsets in the file:
//   0    "20CM"         4 u32 file size     8 u32 size of block A = 8      12 u32 size of block B = 0xF848
//   16   CRC(A)        20 CRC(B)           24 CRC(bytes 0..23)
//   28   block A: u32 0x10D, u32 0xF848
//   36   block B: 0x34 MD5(0x44..0x4033), 0x44 the game's data ('Game' ...), 0x4039 u32 the bank, 0x5A31 the alias's
//        name (ASCII, NUL-padded to 0x5A54), 0xF85C MD5(0x34..0xF85B)
// CRC is the container's checksum (src/app/memcard.ts, Underground 2's too), MD5 RFC 1321's (src/app/md5.ts).
// Re-sealing goes from the inside out (spec section 5): the first MD5, the last, B's CRC, the header's.
//   The bank's place: 0x4039 holds 3,000 in the save of the first safehouse (tests/mw/seed/README.md: Cash 3,000), 0 in
// a new alias's, and grows with the career in the later saves (the nine career seeds, 2026-10-30); taken as the bank.
//   The name field (2026-10-30, a typed "aB3_-.!x y0123..." came out as the folder aB3xy01): letters in either case and
// digits, at most 7; the rest is dropped.  So the rules are Underground 2's, and its texts say them.
//   Not known yet, so not done: where the Save option (Options, Gameplay, Autosave) is in the file (GAME.md 10.4: a new
// saved alias is on Autosave already, and one the player declined to save is not written at all, so the page has
// nothing to turn on), the car records (no mod-car check; M6-27 validates the career's tables at load), and which
// version of the game wrote a file (the spec's saves are v1.3's; any 63,596-byte container of this shape is taken).
//   Pure (no DOM, no OPFS) so it can be checked under node; renameProfile takes the save folder's file functions.
import type * as self from './profile';
import type { Saves } from '../profile';
import type { RenameFiles, SaveFiles } from '../../app/profile';
import { N, t, type Vars } from '../../app/i18n';
import { checksum, isMemcardFile } from '../../app/memcard';
import { md5 } from '../../app/md5';

export { PROFILES_DIR } from './run';
export { checksum };

export const PROFILE_SIZE = 0xF86C;                  // 63,596
const SIZE_A = 8, SIZE_B = 0xF848, VERSION = 0x10D;
const B = 0x24;                                      // block B's start in the file
const MD5_HEAD = 0x34, HEAD_FROM = 0x44, HEAD_TO = 0x4034, MD5_TAIL = 0xF85C;
const MONEY = 0x4039, NAME = 0x5A31, NAME_BYTES = 0x24;

const u32 = (f: Uint8Array, at: number) => new DataView(f.buffer, f.byteOffset, f.byteLength).getUint32(at, true);
const lowerAscii = (s: string) => s.replace(/[A-Z]/g, c => String.fromCharCode(c.charCodeAt(0) + 32));
const same = (a: Uint8Array, b: Uint8Array) => a.length === b.length && a.every((x, i) => x === b[i]);

/** why a check refuses a file, as written (English with {name}s for v): filled for the log, translated for the page */
interface Why { t: string; v?: Vars }

/** A file that starts as the game's save files do ("20CM"): an alias's, or another game's or version's. */
export const isProfileFile = isMemcardFile;

/** The container, its sizes and block A: an alias save of this game, whatever its checks say. */
function checkShape(file: Uint8Array): Why | null {
  if (!isProfileFile(file)) return { t: N('not a profile file (no "20CM")') };
  if (file.length !== PROFILE_SIZE) return { t: N('{n} bytes, not {size}'), v: { n: file.length, size: PROFILE_SIZE } };
  if (u32(file, 4) !== file.length || u32(file, 8) !== SIZE_A || u32(file, 12) !== SIZE_B) return { t: N('unexpected block sizes') };
  if (u32(file, 28) !== VERSION || u32(file, 32) !== SIZE_B) return { t: N('saved by another version of the game (version {got}, not {want})'), v: { got: u32(file, 28), want: VERSION } };
  return null;
}

/** every check the game makes on load (the header's size, magic and three CRCs) and the two MD5s */
const sealedRight = (file: Uint8Array) =>
  u32(file, 16) === checksum(file.subarray(28, B)) && u32(file, 20) === checksum(file.subarray(B)) &&
  u32(file, 24) === checksum(file.subarray(0, 24)) &&
  same(file.subarray(MD5_HEAD, MD5_HEAD + 16), md5(file.subarray(HEAD_FROM, HEAD_TO))) &&
  same(file.subarray(MD5_TAIL, MD5_TAIL + 16), md5(file.subarray(MD5_HEAD, MD5_TAIL)));

/** checkShape, then the checks */
function checkFile(file: Uint8Array): Why | null {
  return checkShape(file) ?? (sealedRight(file) ? null : { t: N('its checksums do not match (edited outside the game?)') });
}

/** the name inside: its bytes up to the first NUL, as the I/O worker reads a path (UTF-8, U+FFFD for bytes that are not) */
function nameInside(file: Uint8Array): string {
  let n = 0;
  while (n < NAME_BYTES && file[NAME + n]) n++;
  return new TextDecoder().decode(file.subarray(NAME, NAME + n));
}

/** Why a file is not an alias the game saved under this name, or null. */
function checkProfile(file: Uint8Array, name: string): Why | null {
  const bad = checkFile(file);
  if (bad) return bad;
  if (lowerAscii(nameInside(file)) !== lowerAscii(name)) return { t: N('the name inside is "{name}"'), v: { name: nameInside(file) } };
  return null;
}

/** An alias's bank; null: not an alias save of this game. */
export const profileMoney = (file: Uint8Array): number | null => checkShape(file) ? null : u32(file, MONEY);

/**
 * The name inside an alias save that comes from elsewhere (the PC game's NFS Most Wanted folder, a backup), or why the
 * page does not take it: the container's shape, and a name the game's own field makes.  edited: its checks do not
 * hold (a save editor's change); it comes in as sealed() makes it.
 */
export function profileName(file: Uint8Array): { name: string; edited: boolean } | { bad: string } {
  const bad = checkShape(file);
  if (bad) return { bad: t(bad.t, bad.v) };
  const name = nameInside(file);
  if (!isProfileName(name)) return { bad: t('the name inside, "{name}", is not one the game makes (1 to {n} letters and digits)', { name, n: NAME_MAX }) };
  return { name, edited: !sealedRight(file) };
}

/** Cars a mod added: Most Wanted's car records are not in the spec yet, so none are counted. */
export const modCars = (_file: Uint8Array): number => 0;

/** The checks made again, in place: the first MD5, the last (which covers it), B's CRC, A's, the header's. */
function seal(f: Uint8Array): Uint8Array {
  f.set(md5(f.subarray(HEAD_FROM, HEAD_TO)), MD5_HEAD);
  f.set(md5(f.subarray(MD5_HEAD, MD5_TAIL)), MD5_TAIL);
  const dv = new DataView(f.buffer, f.byteOffset, f.byteLength);
  dv.setUint32(16, checksum(f.subarray(28, B)), true);
  dv.setUint32(20, checksum(f.subarray(B)), true);
  dv.setUint32(24, checksum(f.subarray(0, 24)), true);
  return f;
}

/** A copy of a save profileName took as edited, its checks made again: as if the game had saved it. */
export const sealed = (file: Uint8Array) => seal(file.slice());

/** A copy of a save the checks accepted, with another name inside (NUL-padded, as the game writes it), sealed again. */
export function withName(file: Uint8Array, name: string): Uint8Array {
  const f = file.slice();
  f.fill(0, NAME, NAME + NAME_BYTES);
  for (let i = 0; i < name.length; i++) f[NAME + i] = name.charCodeAt(i);
  return seal(f);
}

// ------------------------------------------------------------ autosave --

/** Underground 2's list of the profiles it set to Autosave; Most Wanted's page keeps none. */
export const AUTOSAVE_SEEN = 'autosave-profiles.txt';

/** Nothing to do: a new alias the player saves is on Autosave already (GAME.md 10.4), and the option's place in the
 *  file is not known, so the page leaves every alias as the game's Options set it. */
export async function turnOnAutosave(_fs: SaveFiles, _profilesDir: string): Promise<{ turnedOn: string[]; notes: string[] }> {
  return { turnedOn: [], notes: [] };
}

// -------------------------------------------------------------- rename --

/** A name the game's own name field makes: letters and digits, up to 7 of them (see the top). */
export const NAME_MAX = 7;
export const isProfileName = (s: string) => /^[A-Za-z0-9]+$/.test(s) && s.length <= NAME_MAX;

/** No folder belongs to an alias but its own. */
export const coverOf = (_folder: string): { profile: string; cover: string } | null => null;

/**
 * Rename an alias as if the game had made it under the new name: its folder, its file and the name inside (sealed
 * again), and whatever else its folder holds.  Refused, with nothing written, when the new name is not one the game's
 * field makes, when another alias has it (the I/O worker finds names in any case), or when the file fails the checks.
 * A change of case only is a rename too.  The new folder is written in full before the old one goes: a rename that
 * stops halfway leaves the alias as it was.
 */
export async function renameProfile(fs: RenameFiles, profilesDir: string, from: string, to: string):
    Promise<{ files: number } | { refused: string }> {
  if (!isProfileName(to)) return { refused: t('A profile\'s name is 1 to {n} letters and digits, as in the game.', { n: NAME_MAX }) };
  if (to === from) return { files: 0 };
  const folders = new Map<string, string[]>();
  for (const { path } of await fs.listFiles()) {
    const p = path.split('/');
    if (p.length >= 3 && p[0] === profilesDir) folders.set(p[1], [...folders.get(p[1]) ?? [], path]);
  }
  const own = (folder: string, path: string) => { const p = path.split('/'); return p.length === 3 && lowerAscii(p[2]) === lowerAscii(folder); };
  const main = folders.get(from)?.find(path => own(from, path));
  if (!main) return { refused: t('There is no profile {name} in this browser.', { name: from }) };
  const file = await fs.readBytes(main);
  const bad = checkProfile(file, from);
  if (bad) return { refused: t('{name} cannot be renamed here: {why}.', { name: from, why: t(bad.t, bad.v) }) };
  for (const f of folders.keys()) if (f !== from && lowerAscii(f) === lowerAscii(to)) return { refused: t('There is a profile {name} already.', { name: f }) };
  let files = 0;
  try {
    for (const path of folders.get(from)!) {
      const rest = path.slice(profilesDir.length + from.length + 2);
      await fs.writeFile(`${profilesDir}/${to}/${path === main ? to : rest}`, path === main ? withName(file, to) : await fs.readBytes(path));
      files++;
    }
  } catch (e) {
    await fs.remove(`${profilesDir}/${to}`).catch(() => {});
    throw e;
  }
  await fs.remove(`${profilesDir}/${from}`);
  return { files };
}

export type Checked = Saves<typeof self>;
