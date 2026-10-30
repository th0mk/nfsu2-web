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
 *  roam: 16 on phones, 24 on tablets, 32 elsewhere (an iPhone was stopped for memory as 22 friends' cars loaded at
 *  once at the new career's start, 2026-10-25). */
export function deviceDefaults(): Record<string, string> {
  if (isPhone()) return { render: rememberedRender() === 'backing' ? 'backing' : 'scale:0.75', fullscreen: 'on', hud: '43', roam_max_cars: '16' };
  return touchFirst() ? { roam_max_cars: '24' } : {};
}

import { STORAGE } from '@game/storage';

/** the render check's verdict on this phone: in this browser only, like the other device defaults (settings.ini,
 *  and with it the cloud saves, never has it) */
const RENDER_KEY = STORAGE.LOCAL.RENDER_CHECK;
export function rememberedRender(): string | null {
  try { return localStorage.getItem(RENDER_KEY); } catch { return null; }
}
export function rememberRender(v: string | null) {
  try { if (v) localStorage.setItem(RENDER_KEY, v); else localStorage.removeItem(RENDER_KEY); }
  catch { /* storage off: the check measures again next time */ }
}

/** whether the browser shows the page a controller (Chrome: only after a button press on the page) */
export function padSeen(): boolean {
  try { return typeof navigator.getGamepads === 'function' && navigator.getGamepads().some(g => !!g?.connected); }
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
