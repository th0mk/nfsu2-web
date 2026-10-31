// The header's Changelog: a popup with the releases of CHANGELOG.md, newest first, as web/build.mjs writes them to
// /changelog.json.  Fetched the first time it opens, again after a failure.  The changes stay in English (lang="en");
// the popup's own words and the releases' dates follow the page's language.

import { dateLocale, onLanguage, t } from './i18n';

/** one release: its date (yyyy-mm-dd), the last commit it brings, and its changes as [area, text] ('' area: none) */
type Release = { date: string; commit: string; changes: [string, string][] };

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
      const ul = Object.assign(document.createElement('ul'), { lang: 'en' });
      for (const [area, text] of r.changes) {
        const li = ul.appendChild(document.createElement('li'));
        if (area) li.append(Object.assign(document.createElement('strong'), { textContent: `${area}:` }), ' ');
        li.append(text);
      }
      return [h, ul];
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
