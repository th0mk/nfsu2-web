// The header's Changelog: a popup with the releases of CHANGELOG.md, newest first, as web/build.mjs writes them to
// /changelog.json, each with its new things and, folded under them, its fixes.  Fetched the first time it opens, again
// after a failure.  The changes stay in English (lang="en"); the popup's own words and the releases' dates follow the
// page's language.

import { dateLocale, onLanguage, t, tn } from './i18n';

type Change = [area: string, text: string];
/** one release: its date (yyyy-mm-dd), the last commit it brings, its new things and its fixes ('' area: none) */
type Release = { date: string; commit: string; changes: Change[]; fixes: Change[] };

function list(changes: Change[]) {
  const ul = Object.assign(document.createElement('ul'), { lang: 'en' });
  for (const [area, text] of changes) {
    const li = ul.appendChild(document.createElement('li'));
    if (area) li.append(Object.assign(document.createElement('strong'), { textContent: `${area}:` }), ' ');
    li.append(text);
  }
  return ul;
}

export function changelog(button: HTMLElement, dialog: HTMLDialogElement, body: HTMLElement) {
  let releases: Release[] | null = null, loading: Promise<void> | null = null;
  const draw = () => {
    if (!releases) return;
    const year = new Date().getFullYear();
    body.replaceChildren(...releases.flatMap(r => {
      const d = new Date(`${r.date}T12:00:00Z`);
      const h = Object.assign(document.createElement('h3'), {
        textContent: d.toLocaleDateString(dateLocale(), { day: 'numeric', month: 'long', timeZone: 'UTC', ...(d.getUTCFullYear() === year ? {} : { year: 'numeric' }) }),
      });
      const out: HTMLElement[] = [h];
      if (r.changes.length) out.push(list(r.changes));
      if (r.fixes.length) {
        const fixes = document.createElement('details');
        fixes.append(Object.assign(document.createElement('summary'), { textContent: tn(r.fixes.length, '{n} fix', '{n} fixes') }), list(r.fixes));
        out.push(fixes);
      }
      return out;
    }));
  };
  const load = async () => {
    const r = await fetch('/changelog.json', { cache: 'no-cache' });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    releases = await r.json() as Release[];
    draw();
  };
  button.onclick = () => {
    dialog.showModal();
    loading ??= load().catch(() => {
      loading = null;
      body.replaceChildren(Object.assign(document.createElement('p'), { className: 'hint', textContent: t("Couldn't load the changelog.") }));
    });
  };
  onLanguage(draw);
}
