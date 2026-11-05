// /lastfm.html: where Last.fm's "allow access" page sends the player back, with a token, in the popup the start page opened
// (app/lastfm.ts).  The token is traded for the session key (the server's /lastfm), which goes where the start page reads it; the
// start page sees the change (a storage event) and says what came of it.  The popup then closes; a page that is no popup (a phone's
// browser may open one as a tab) goes back to the start page's Game tab.
import { keepSession, RESULT_KEY } from './app/lastfm';

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

void connect().then(result => {
  try { localStorage.setItem(RESULT_KEY, JSON.stringify({ result, at: Date.now() })); } catch { /* a private window keeps nothing */ }
  if (window.opener) window.close();
  location.replace('/#game');
});
