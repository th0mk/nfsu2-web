// The graphics a phone starts with.  A new player's registry.txt is the built-in seed (app/registry-defaults.txt, the
// game's highest settings, shared with the Mac app); on a phone it has car and road reflections, motion blur, depth of
// field, overbright and bleach bypass off.  Measured on the PC over one lockstep Quick Race (2026-10-26): draws per
// frame 1,639 -> 1,118, render passes 30 -> 4, GPU time -54 %, WebGPU encoding -36 %.  Rain, fog, particles, light
// trails and world detail stay.  A phone that started before this gets them once too, if every graphics option there
// still has the seed's value; one where any was changed keeps its own.  The game's Options, Display raise them again.
// Cloud saves leave registry.txt out (vault.ts keeps), so a phone's never reaches a PC.
//
// A computer's new registry has the game's Full Scene Anti-Aliasing on (runtime/com.c MULTISAMPLING: 4 samples, about
// 1 to 1.5 ms more GPU time a frame at 2560x1440 on an M4 Pro, 2026-11-08); tablets' and phones' stay as the seed
// (off; phones are not offered it).  A computer's registry from before, which could never have it (-1), is asked about
// it once at Play (shell.ts askFsaa) and marked.

/** the game's own options (registry DWORDs) a phone starts with */
export const PHONE_GRAPHICS: Readonly<Record<string, number>> = {
  g_CarEnvironmentMapEnable: 0, g_RoadReflectionEnable: 0, g_MotionBlurEnable: 0,
  g_DepthOfFieldEnable: 0, g_OverBrightEnable: 0, g_BleachByPassEnable: 0,
};
/** a value of ours in registry.txt (the game never asks for it, and win32_ext.c keeps every line it read): this
 *  registry has had its phone graphics, or was left alone, and is not looked at again */
export const PHONE_MARK = 'PhoneGraphics';

const DWORD = /^d ([^=\r\n]+)=(\d+)(\r?)$/gm;
function dwords(text: string): Map<string, number> {
  return new Map([...text.matchAll(DWORD)].map(m => [m[1].toLowerCase(), +m[2]]));
}

/** the registry text with these DWORDs set: each line replaced where it is, or added at the end */
export function withDwords(text: string, values: Record<string, number>): string {
  const left = new Map(Object.entries(values).map(([k, v]) => [k.toLowerCase(), [k, v] as const]));
  let out = text.replace(DWORD, (line, name: string, _v: string, cr: string) => {
    const kv = left.get(name.toLowerCase());
    if (!kv) return line;
    left.delete(name.toLowerCase());
    return `d ${name}=${kv[1]}${cr}`;
  });
  const nl = text.includes('\r\n') ? '\r\n' : '\n';
  if (left.size && out && !out.endsWith('\n')) out += nl;
  for (const [k, v] of left.values()) out += `d ${k}=${v}${nl}`;
  return out;
}

/** the registry a phone starts with */
export function phoneSeed(seed: string): string {
  return withDwords(seed, { ...PHONE_GRAPHICS, [PHONE_MARK]: 1 });
}

/** the game's Full Scene Anti-Aliasing level a computer starts with: on (the option's one step) */
export const COMPUTER_GRAPHICS: Readonly<Record<string, number>> = { g_FSAALevel: 1 };

/** a value of ours in registry.txt: the start page asked this player about Full Scene Anti-Aliasing (shell.ts askFsaa) */
export const FSAA_MARK = 'FsaaAsked';
/** g_FSAALevel -1 as the game writes it: what every registry had while the browser game offered no multisampling */
const FSAA_NEVER = 4294967295;

/** a registry from before Full Scene Anti-Aliasing (still -1, never chosen) whose player was not asked yet */
export function fsaaAsk(text: string): boolean {
  const have = dwords(text);
  return !have.has(FSAA_MARK.toLowerCase()) && have.get('g_fsaalevel') === FSAA_NEVER;
}

/** the registry after the question: Full Scene Anti-Aliasing on (its one step) or left as it was, and the mark */
export function fsaaAnswer(text: string, on: boolean): string {
  return withDwords(text, on ? { ...COMPUTER_GRAPHICS, [FSAA_MARK]: 1 } : { [FSAA_MARK]: 1 });
}

/** a new player's registry on this kind of device (device.ts deviceKind) */
export function deviceSeed(kind: 'phone' | 'tablet' | 'pc', seed: string): string {
  return kind === 'phone' ? phoneSeed(seed) : kind === 'pc' ? withDwords(seed, COMPUTER_GRAPHICS) : seed;
}

/** a phone's registry from before the phone graphics: the text to write (the phone graphics when every graphics option,
 *  the seed's g_ values but the resolution, still has the seed's value; else only the mark), or null once it has the mark */
export function phoneUpgrade(text: string, seed: string): { text: string; lowered: boolean } | null {
  const have = dwords(text);
  if (have.has(PHONE_MARK.toLowerCase())) return null;
  const untouched = [...dwords(seed)].every(([k, v]) => !k.startsWith('g_') || k === 'g_racingresolution' || have.get(k) === v);
  return untouched
    ? { text: withDwords(text, { ...PHONE_GRAPHICS, [PHONE_MARK]: 1 }), lowered: true }
    : { text: withDwords(text, { [PHONE_MARK]: 1 }), lowered: false };
}
