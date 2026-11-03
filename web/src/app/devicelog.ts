// The browser and the device as the log names them (the session lines, shell.ts): enough to tell what ran and what it
// had to run on, nothing that tells who (the audit's PRIV-5: the browser's major version and the system, no more).

/** "Chrome 155 on Windows", "Safari 26 on iOS", "Safari 26 on iPadOS" (an iPad's Safari says Macintosh, but has touch),
 *  "Safari 26 on iOS (desktop website)" (an iPhone's Safari asked for desktop websites: Macintosh, touch, a phone's
 *  screen; phone: device.ts phoneScreen), "Chrome 156 on Android 11" (android: the major version, androidMajor) */
export function browserBrief(ua: string, touchPoints = 0, phone = false, android?: string): string {
  const mac = /Macintosh/.test(ua) && touchPoints > 1;
  const ios = /iPhone|iPod/.test(ua) ? 'iOS' : mac && phone ? 'iOS (desktop website)' : /iPad/.test(ua) || mac ? 'iPadOS' : null;
  const os = ios ?? (/CrOS/.test(ua) ? 'ChromeOS' : /Windows/.test(ua) ? 'Windows' : /Mac OS X|Macintosh/.test(ua) ? 'macOS'
    : /Android/.test(ua) ? 'Android' : /Linux/.test(ua) ? 'Linux' : 'another system');
  const v = (re: RegExp) => re.exec(ua)?.[1];
  const safari = v(/Version\/(\d+)[\d.]* (?:Mobile\/\S+ )?Safari\//);
  let name: string;
  // every browser on iOS and iPadOS is WebKit underneath: Chrome, Firefox and Edge there say CriOS, FxiOS and EdgiOS
  if (ios) name = v(/CriOS\/(\d+)/) ? `Chrome ${v(/CriOS\/(\d+)/)} (WebKit)` : v(/FxiOS\/(\d+)/) ? `Firefox ${v(/FxiOS\/(\d+)/)} (WebKit)`
    : v(/EdgiOS\/(\d+)/) ? `Edge ${v(/EdgiOS\/(\d+)/)} (WebKit)` : safari ? `Safari ${safari}` : 'another browser (WebKit)';
  else name = v(/Firefox\/(\d+)/) ? `Firefox ${v(/Firefox\/(\d+)/)}` : v(/(?:Chrome|Chromium)\/(\d+)/) ? `Chrome ${v(/(?:Chrome|Chromium)\/(\d+)/)}`
    : safari ? `Safari ${safari}` : 'another browser';
  return `${name} on ${os}${os === 'Android' && android && /^\d+$/.test(android) ? ` ${android}` : ''}`;
}

/** The Android version's major number, where the browser says it: Chrome's user agent says "Android 10" on every
 *  version since 110 (reduced), but its client hints give the real one.  Some Adreno phones' graphics drivers fail
 *  where others run (2026-10/11, game.ts GPU-ANDROID-DRIVER): the version tells the old ones apart.  The major
 *  number only (the audit's PRIV-5); undefined elsewhere, or when the browser does not answer within a second. */
export async function androidMajor(): Promise<string | undefined> {
  const d = (navigator as { userAgentData?: { platform?: string; getHighEntropyValues?(h: string[]): Promise<{ platformVersion?: string }> } }).userAgentData;
  if (d?.platform !== 'Android' || !d.getHighEntropyValues) return undefined;
  try {
    const v = await Promise.race([d.getHighEntropyValues(['platformVersion']), new Promise<undefined>(r => setTimeout(r, 1000))]);
    return /^(\d+)/.exec(v?.platformVersion ?? '')?.[1];
  } catch { return undefined; }
}

export interface DeviceFacts {
  /** the screen in CSS pixels and the device pixels to one */
  screenW: number; screenH: number; dpr: number;
  /** navigator.hardwareConcurrency, navigator.deviceMemory (Chrome only; Safari has none) */
  cores?: number; memoryGB?: number;
  touchPoints: number;
  /** started from the Home Screen (no browser bars), not in a browser tab */
  homeScreen: boolean;
  /** the refresh rate the page measured before Play */
  displayHz: number;
}

/** "screen 428x926 at 3x, 6 cores, touch, browser tab"; a 30 Hz display before Play is most likely Low Power Mode */
export function deviceBrief(d: DeviceFacts): string {
  const parts = [`screen ${d.screenW}x${d.screenH} at ${+d.dpr.toFixed(2)}x`];
  if (d.cores) parts.push(`${d.cores} cores`);
  if (d.memoryGB) parts.push(`${d.memoryGB} GB memory`);
  parts.push(d.touchPoints > 1 ? 'touch' : 'no touch', d.homeScreen ? 'Home Screen app' : 'browser tab');
  if (d.displayHz > 0 && d.displayHz <= 31) parts.push(`the display at ${d.displayHz} Hz before Play (Low Power Mode, or a window in the background)`);
  return parts.join(', ');
}
