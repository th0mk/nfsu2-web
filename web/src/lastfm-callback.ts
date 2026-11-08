// /lastfm.html: where Last.fm's "allow access" page sends the player back, with a token, in the popup the start page opened
// (app/lastfm.ts).  The token is traded for the session key (the server's /lastfm), which goes where the start page reads it; the
// start page sees the change (a storage event) and says what came of it.  The popup then closes.  Last.fm's pages cut the popup's
// link to the start page (window.opener is null by then), so this never loads the start page itself: a browser that keeps the
// window open gets one line saying it can be closed.
import { keepSession } from './app/lastfm';
import { LASTFM_RESULT, localStore } from './app/prefs';
import { lastLanguage, setLanguage, t } from './app/i18n';

async function connect(): Promise<string> {
  const token = new URLSearchParams(location.search).get('token');
  if (!token) return 'token';
  try {
    const r = await fetch('/lastfm', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op: 'session', token }) });
    const j = await r.json() as { name?: unknown; sk?: unknown; error?: unknown };
    if (r.ok && typeof j.name === 'string' && typeof j.sk === 'string') { keepSession({ name: j.name, sk: j.sk }); return 'ok'; }
    return typeof j.error === 'string' ? j.error : 'down';
  } catch { return 'down'; }
}

void connect().then(async result => {
  localStore.setJSON(LASTFM_RESULT, { result, at: Date.now() });
  window.close();
  await setLanguage(lastLanguage()).catch(() => false);
  const p = Object.assign(document.createElement('p'), { className: 'hint lastfm-closed',
    textContent: result === 'ok' ? t('Last.fm connected. You can close this window.') : t('Last.fm didn\'t connect. Close this window and try again.') });
  document.body.append(p);
});
