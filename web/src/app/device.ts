// What kind of device the page runs on, for the defaults and the hints that
// differ on phones and touch screens.
//   phone:       the browser says it is a mobile one (Chrome's User-Agent
//                Client Hints; "Mobile" or "iPhone" in the user agent of the
//                others), or it asks for desktop websites (Safari's Request
//                Desktop Website, Chrome's Desktop site: Macintosh or Linux,
//                no "Mobile") on a touch screen whose short side is a phone's
//                (under 600 CSS px; an iPad mini's is 744).  Tablets are not
//                phones.
//   touch first: the primary pointer is a finger (phones, tablets), not a
//                laptop whose screen also takes touch.
// The Controls sheet and the hints follow touch first (html[data-touch]).
import { allPads } from '../input/steampad';

export function isPhone(): boolean {
  const uad = (navigator as unknown as { userAgentData?: { mobile?: boolean } }).userAgentData;
  return uad?.mobile === true || /Android.+Mobile|iPhone|iPod/.test(navigator.userAgent) || phoneScreen(navigator.maxTouchPoints);
}

/** a touch screen with a phone's short side (a phone whatever its browser says), from the screen in CSS px */
export function phoneScreen(touchPoints: number, w = screen.width, h = screen.height): boolean {
  return touchPoints > 1 && Math.min(w, h) < 600;
}

export function touchFirst(): boolean { return matchMedia('(pointer: coarse)').matches; }

/** settings.ini keys whose default is another on this device: what applies when neither the file nor the page
 *  address sets them.  Phones: 75% resolution scale (a phone's screen has more pixels than its GPU fills at 60 fps),
 *  or 100% once the render check found room to spare on this one (rendercheck.ts), full screen (no browser bars
 *  over the game), and the game's 4:3 HUD: on a phone's long screen the wide one puts the minimap under the touch
 *  controls' steering and the speedometer under Brake, the 4:3 one between the thumbs.  Players' cars shown in free
 *  roam: 16 on phones, 24 on tablets (an iPhone was stopped for memory as 22 friends' cars loaded at once at the new
 *  career's start, 2026-10-25), Auto on computers: 32, and 48 or 64 while the game's own frames show room for them
 *  with more players near (roam_game.c cars_auto: each car costs the game's thread about 0.28 ms a frame on a Ryzen
 *  5900X, a laptop's several times that, so no computer gets more up front: fast computers started at 48, then 40,
 *  until 2026-11-02, and lost a fifth of their frames on fast displays).  The
 *  world map's other players: 2048 on phones, the file's 4096 elsewhere (each marker costs the open map's frames,
 *  about 7 us a frame on a busy Ryzen 5900X, a phone's several times that).  Full screen stays off on phones with
 *  PowerVR's B-Series graphics (fullScreenFails). */
export function deviceDefaults(): Record<string, string> {
  const kind = deviceKind();
  if (kind === 'phone') return { render: rememberedRender() === 'backing' ? 'backing' : 'scale:0.75', fullscreen: fullScreenFails() ? 'off' : 'on', hud: '43', roam_max_cars: '16', roam_map_max: '2048', coop: 'off' };
  if (kind === 'tablet') return { roam_max_cars: '24', coop: 'off' };
  return { roam_max_cars: 'auto' };
}

/** the graphics as the page's check named them (checks.ts checkGpu: "img-tec b-series ..."), '' until it has */
let graphics = '';
export function noteGraphics(info: string | undefined) { graphics = info ?? ''; }
/** PowerVR's B-Series in Chrome (POCO M7 Pro, Honor 400 Lite and others, 2026-10/11): in full screen the graphics
 *  stopped seconds after Play or a turn to landscape (vkBindImageMemory: VK_ERROR_OUT_OF_DEVICE_MEMORY, with the
 *  canvas the whole screen), in the window the game ran (a player's: "without full screen game works nice") */
export const fullScreenFails = (g = graphics) => /^img-tec b-series/i.test(g);

/** the device as its defaults see it: a phone, a touch-first one that is not a phone (a tablet), or a computer */
export function deviceKind(): 'phone' | 'tablet' | 'pc' {
  return isPhone() ? 'phone' : touchFirst() ? 'tablet' : 'pc';
}

/** The guest heap's window (runtime/guest_span.h): the game's memory 2 GB from the start, 768 MB more of it for the
 *  game's own heap than its 512 MB, which a crowded free roam fills on its trips out and back (450-507 MB measured,
 *  2026-11-04).  On a computer only: a phone's or tablet's Chrome for Android is a 32-bit program on most such devices
 *  and sets aside 1 GB at most (host/memory.ts), and Safari keeps every buffer in one area that a few large memories
 *  use up (WebKit bug 269937).  Not where the browser says the device has less than 8 GB (Chrome and Edge say 8 at
 *  most; Firefox says nothing): the memory counts against the computer's commit limit on Windows. */
export function heapWindow(kind = deviceKind(), ua = navigator.userAgent,
                           memoryGB = (navigator as unknown as { deviceMemory?: number }).deviceMemory,
                           platform = (navigator as unknown as { userAgentData?: { platform?: string } }).userAgentData?.platform): boolean {
  const safari = /AppleWebKit\//.test(ua) && !/Chrome\/|Chromium\/|Firefox\//.test(ua);
  // (an Android tablet with a mouse, or a phone on a desktop screen (DeX), reads as a computer: its platform says Android)
  const android = platform === 'Android' || /Android/.test(ua);
  return kind === 'pc' && !safari && !android && (memoryGB === undefined || memoryGB >= 8);
}

/** the render check's verdict on this phone: in this browser only, like the other device defaults (settings.ini,
 *  and with it the cloud saves, never has it) */
const RENDER_KEY = 'nfsu2.device.render';
export function rememberedRender(): string | null {
  try { return localStorage.getItem(RENDER_KEY); } catch { return null; }
}
export function rememberRender(v: string | null) {
  try { if (v) localStorage.setItem(RENDER_KEY, v); else localStorage.removeItem(RENDER_KEY); }
  catch { /* storage off: the check measures again next time */ }
}

/** whether the browser shows the page a controller (Chrome: only after a button press on the page) */
export function padSeen(): boolean {
  try { return allPads().some(g => !!g?.connected); }
  catch { return false; }
}

/** html[data-touch] while the primary pointer is a finger (the Controls sheet and the hints show what fits) */
export function markTouch() {
  const q = matchMedia('(pointer: coarse)');
  const set = () => document.documentElement.toggleAttribute('data-touch', q.matches);
  set();
  q.addEventListener('change', set);
}

/** no pinch zoom on an iPhone: Safari ignores the viewport's user-scalable=no, and cancelling its own gesture events
 *  is what stops a pinch there (style.css's touch-action stops the double tap) */
export function noPinchZoom() {
  for (const t of ['gesturestart', 'gesturechange']) document.addEventListener(t, e => e.preventDefault(), { passive: false });
}
