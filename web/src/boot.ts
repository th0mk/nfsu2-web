// The start page's word when the game's own script never starts.  An antivirus or a network filter that blocks
// main.js (Kaspersky's false positive of 2 November 2026) leaves index.html on "Loading the game." with nothing of
// ours running, which looks like the site is broken.  This small classic script runs first (deferred, before main.js
// in the page) and, when main.js fails to load or has not started WAIT_MS after the page opened, says so in the
// page's language, with Reload.  main.js marks its start (app/started.ts): before that time, nothing shows; after it
// (a slow network), the page goes back to what it showed.  Its texts are the page's (lang/*.json): build.mjs puts
// these few in (i18n-keys.mjs bootTexts), as this cannot fetch a language file.

declare const BOOT_TEXTS: Record<string, Record<string, string>>;
export {};

const WAIT_MS = 15_000;
const N = (s: string) => s;
const started = () => document.documentElement.hasAttribute('data-started');

let shown = false;
function show() {
  if (shown || started()) return;
  const status = document.getElementById('status'), sub = document.getElementById('status-sub'), reload = document.getElementById('reload');
  if (!status || !sub || !reload) return;
  shown = true;
  let lang = 'english';
  try { lang = localStorage.getItem('nfsu2-page-language') || lang; } catch { /* no storage: English, as the page */ }
  const t = (k: string) => BOOT_TEXTS[lang]?.[k] ?? k;
  const was = [status.textContent, sub.textContent, reload.textContent, reload.hidden] as const;
  status.textContent = t(N('Not loading'));
  sub.textContent = t(N('Your antivirus or network is probably blocking the game. Allow {site} in it, then reload.')).replace('{site}', location.hostname);
  reload.textContent = t(N('Reload'));
  reload.hidden = false;
  reload.addEventListener('click', () => { if (!started()) location.reload(); });
  addEventListener('nfsu2-started', () => { [status.textContent, sub.textContent, reload.textContent, reload.hidden] = was; }, { once: true });
}

// main.js not loading at all (blocked, or an antivirus's own page in its place): at once
addEventListener('error', e => {
  const s = e.target;
  if (s instanceof HTMLScriptElement && new URL(s.src, location.href).pathname === '/main.js') show();
}, true);
// main.js loaded but never started (replaced by an empty script), or still on its way
setTimeout(show, Math.max(0, WAIT_MS - performance.now()));
