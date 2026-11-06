// A controller in the page's own dialogs (the menu, the controls sheet, the
// Quit question) while a game runs: the input module hands the page the
// button presses while one is open (gamepad.ts PadPage), and the game gets
// the controller at rest meanwhile.
//   D-pad or left stick  the focus to the next or previous button
//   A                    presses it
//   B, Start             closes the dialog, as Esc does (B on the menu's Crews or
//                        Friends: back to its list, as its ‹ does)

import { PAD_A, PAD_B, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT, PAD_DUP, PAD_START } from '../input/proto';

/** the open modal dialog of the page (of two, the one on top: the one later in the page, as the playlist after the
 *  menu), or null */
export function pageDialog(): HTMLDialogElement | null {
  const open = document.querySelectorAll<HTMLDialogElement>('dialog[open]:modal');
  return open[open.length - 1] ?? null;
}

/** a dialog the controller works: its focus ring shows (data-pad), until it closes */
export function markPad(d: HTMLDialogElement) {
  if (d.hasAttribute('data-pad')) return;
  d.setAttribute('data-pad', '');
  d.addEventListener('close', () => d.removeAttribute('data-pad'), { once: true });
}

const FOCUSABLE = 'button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), a[href]';

export function padPress(button: number) {
  const d = pageDialog();
  if (!d) return;
  markPad(d);
  const shown = (e: HTMLElement) => e.checkVisibility?.() ?? e.offsetParent !== null;
  if (button === PAD_B || button === PAD_START) {
    // (a page of the dialog in its place, the menu's Crews or Friends: its Back, data-act="back", shell.ts menuPage)
    const back = button === PAD_B ? [...d.querySelectorAll<HTMLElement>('[data-act="back"]')].find(shown) : undefined;
    if (back) back.click(); else closeLikeEsc(d);
    return;
  }
  const items = [...d.querySelectorAll<HTMLElement>(FOCUSABLE)].filter(shown);
  const at = items.indexOf(document.activeElement as HTMLElement);
  if (button === PAD_A) { if (at >= 0) items[at].click(); return; }
  const step = button === PAD_DUP || button === PAD_DLEFT ? -1 : button === PAD_DDOWN || button === PAD_DRIGHT ? 1 : 0;
  if (!step || !items.length) return;
  const next = items[at < 0 ? 0 : Math.max(0, Math.min(items.length - 1, at + step))];
  next.focus({ focusVisible: true } as FocusOptions);
}

/** Esc's way out: a cancel event the dialog may refuse, then the close */
function closeLikeEsc(d: HTMLDialogElement) {
  const rc = (d as { requestClose?(): void }).requestClose;
  if (typeof rc === 'function') { rc.call(d); return; }
  if (d.dispatchEvent(new Event('cancel', { cancelable: true }))) d.close();
}
