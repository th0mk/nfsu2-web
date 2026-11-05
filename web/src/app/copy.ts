// The page's copy buttons (an invite link, the passphrase, a friend's Discord username): the text onto the clipboard
// (copyText), and a small toast right at the button that says so (copyToast), gone after a moment.
import { t } from './i18n';

/** how long a toast stays (ms); one the player has to read (a name the browser would not copy) longer */
export const TOAST_MS = 1600;
export const READ_MS = 4000;

/** Text onto the clipboard: the Clipboard API, else the old way (an older WebView, a page without the focus), in the
 *  dialog of button `at` when it is in one (the page under a modal dialog takes no selection).  False: neither did. */
export async function copyText(text: string, at?: Element): Promise<boolean> {
  try { await navigator.clipboard.writeText(text); return true; } catch { /* the old way, then */ }
  const ta = document.createElement('textarea'); ta.value = text; ta.setAttribute('readonly', ''); ta.style.cssText = 'position:fixed;opacity:0';
  const back = document.activeElement as HTMLElement | null;
  (at?.closest('dialog') ?? document.body).append(ta); ta.select();
  let ok = false;
  try { ok = document.execCommand('copy'); } catch { /* none */ }
  ta.remove();
  back?.focus?.({ preventScroll: true });
  return ok;
}

/** the toast each button has now (one at a time a button: a second click puts a new one in its place) */
const shown = new WeakMap<Element, HTMLElement>();

/** A small toast at button `at`: over its middle (under it where there is no room above), inside the window, for
 *  `ms`.  In the top layer (a popover) where the browser has one, so a button in a modal dialog has it over the
 *  dialog; else in the button's dialog or the page. */
export function copyToast(at: HTMLElement, text = t('Copied'), ms = TOAST_MS) {
  shown.get(at)?.remove();
  const el = document.createElement('div');
  el.className = 'copy-toast';
  el.setAttribute('role', 'status');
  el.textContent = text;
  const layer = 'showPopover' in el;
  if (layer) el.popover = 'manual';
  (layer ? document.body : at.closest('dialog') ?? document.body).append(el);
  if (layer) el.showPopover();
  const b = at.getBoundingClientRect(), { width: w, height: h } = el.getBoundingClientRect(), gap = 8, edge = 8;
  const above = b.top - gap - h >= edge;
  el.style.left = `${Math.floor(Math.max(edge, Math.min(b.left + b.width / 2 - w / 2, innerWidth - w - edge)))}px`;
  el.style.top = `${Math.round(above ? b.top - gap - h : b.bottom + gap)}px`;
  el.classList.toggle('below', !above);
  shown.set(at, el);
  requestAnimationFrame(() => el.classList.add('on'));
  setTimeout(() => {
    el.classList.remove('on');
    setTimeout(() => { el.remove(); if (shown.get(at) === el) shown.delete(at); }, 200);
  }, ms);
}
