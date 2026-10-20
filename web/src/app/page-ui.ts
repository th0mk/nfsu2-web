// The start page's own furniture, around the sections the other modules fill:
// the tabs (Online, Settings, Saves; Controls and What's new in the help
// dialog), the top bar's buttons, Play in the top bar once the big one has
// scrolled away, the players line in the status header, and the controller
// card (connected or not, a rumble test).  None of it changes the game or
// its files.

const $ = <T extends HTMLElement = HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

export interface Tabs { select(pane: string, focus?: boolean): void; selected(): string }

/** A tab list: each [role=tab] shows the pane its aria-controls names and hides the others.  Arrow keys, Home and
 *  End move between the tabs (a roving tab stop).  remember: the localStorage key that keeps the choice (a
 *  per-viewer convenience: the page works the same without it). */
export function tabList(list: HTMLElement, remember?: string): Tabs {
  const tabs = [...list.querySelectorAll<HTMLButtonElement>('[role="tab"]')];
  const pane = (t: HTMLElement) => t.getAttribute('aria-controls') ?? '';
  const select = (t: HTMLButtonElement | undefined, focus = false) => {
    if (!t) return;
    for (const x of tabs) {
      const on = x === t;
      x.setAttribute('aria-selected', String(on));
      x.tabIndex = on ? 0 : -1;
      const p = document.getElementById(pane(x));
      if (p) p.hidden = !on;
    }
    if (focus) t.focus();
    if (remember) try { localStorage.setItem(remember, pane(t)); } catch { /* storage refused: not kept */ }
  };
  for (const t of tabs) t.addEventListener('click', () => select(t));
  list.addEventListener('keydown', e => {
    const i = tabs.indexOf(document.activeElement as HTMLButtonElement);
    if (i < 0) return;
    const to = e.key === 'ArrowRight' ? (i + 1) % tabs.length : e.key === 'ArrowLeft' ? (i + tabs.length - 1) % tabs.length
      : e.key === 'Home' ? 0 : e.key === 'End' ? tabs.length - 1 : -1;
    if (to < 0) return;
    e.preventDefault();
    select(tabs[to], true);
  });
  if (remember) {
    let kept: string | null = null;
    try { kept = localStorage.getItem(remember); } catch { /* storage refused */ }
    const t = tabs.find(x => pane(x) === kept);
    if (t) select(t);
  }
  return {
    select: (id, focus = false) => select(tabs.find(x => pane(x) === id), focus),
    selected: () => pane(tabs.find(x => x.getAttribute('aria-selected') === 'true') ?? tabs[0]),
  };
}

export interface PageUi {
  panel: Tabs;
  help: Tabs;
  /** the help dialog, at Controls or What's new */
  openHelp(pane?: 'help-body' | 'changes'): void;
  /** the players line in the status header ('' hides it) */
  here(text: string): void;
  /** how many profiles the Saves tab counts (null: not counted) */
  profiles(n: number | null): void;
}

/** Wire the page's furniture.  play: what Play does; say: a short message at the bottom of the page. */
export function pageUi(play: () => void, say: (msg: string) => void): PageUi {
  const panel = tabList($('#panel .tabs'), 'nfsu2-tab');
  const help = tabList($('#help .tabs'));
  const openHelp = (p: 'help-body' | 'changes' = 'help-body') => {
    help.select(p);
    const d = $<HTMLDialogElement>('#help');
    if (!d.open) d.showModal();
  };
  $('#open-help').onclick = () => openHelp('help-body');
  $('#open-controls').onclick = () => openHelp('help-body');
  $('#open-settings').onclick = () => {
    panel.select('settings', true);
    $('#panel').scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
  };

  // Play in the top bar while the big one is out of sight (it mirrors its disabled state: shell.ts updatePlay)
  const top = $<HTMLButtonElement>('#top-play');
  top.onclick = play;
  if ('IntersectionObserver' in window) {
    new IntersectionObserver(([e]) => {
      const show = !e.isIntersecting;
      top.classList.toggle('show', show);
      top.tabIndex = show ? 0 : -1;
      top.toggleAttribute('aria-hidden', !show);
    }, { rootMargin: '-60px 0px 0px 0px' }).observe($('#play'));
  }

  // the controller card: whether one is connected (the browser tells only after a button press), a rumble test
  const pill = $('#pad-pill');
  const name = (g: Gamepad) => g.id.replace(/\s*\(.*$/, '').trim() || 'Controller';
  const showPads = () => {
    const g = [...(navigator.getGamepads?.() ?? [])].find((x): x is Gamepad => !!x);
    pill.className = g ? 'pill' : 'pill off';
    pill.textContent = g ? `Connected: ${name(g)}` : 'No controller';
    pill.title = g ? g.id : '';
  };
  window.addEventListener('gamepadconnected', showPads);
  window.addEventListener('gamepaddisconnected', showPads);
  showPads();
  $('#rumble-test').onclick = () => {
    const pads = $('#pads');
    pads.classList.remove('rumble'); void pads.offsetWidth; pads.classList.add('rumble');
    const g = [...(navigator.getGamepads?.() ?? [])].find((x): x is Gamepad => !!x?.vibrationActuator);
    if (g?.vibrationActuator) {
      void g.vibrationActuator.playEffect('dual-rumble', { duration: 400, strongMagnitude: 0.8, weakMagnitude: 0.6 }).catch(() => {});
      say(`Rumble sent to ${name(g)}.`);
    } else say([...(navigator.getGamepads?.() ?? [])].some(Boolean) ? 'This controller has no rumble the browser can use.'
      : 'Connect a controller and press any of its buttons.');
  };

  return {
    panel, help, openHelp,
    here(text) { $('#here').hidden = !text; $('#here-text').textContent = text; },
    profiles(n) { const c = $('#saves-count'); c.hidden = n === null; c.textContent = n === null ? '' : String(n); },
  };
}
