// Play's questions show over the stage since cae4d8df (the sound and full screen first, inside the click): one that
// throws must not leave the player there.  A bare page (no test settings in its address, so Play asks the free-roam
// question), the origin's storage made full before Play (Storage.overrideQuotaForOrigin), Not now: the question's
// settings.ini write fails.  Safe: the start page is back (no stage, the page not inert).  On cae4d8df the stage stays up
// over an inert page with no game: only a reload gets out.
//   node web/tools/play-question-fails.mjs --base http://127.0.0.1:<port>
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';

const opt = n => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : null; };
const base = opt('--base') ?? 'http://127.0.0.1:8614';
const browser = await chromium.launch({ ...pickBrowser(), headless: true,
  args: ['--enable-unsafe-webgpu', '--autoplay-policy=no-user-gesture-required', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] });
const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 } });
const page = await ctx.newPage(), cdp = await ctx.newCDPSession(page);
const errors = [];
page.on('pageerror', e => errors.push(String(e)));
await page.goto(`${base}/`);
await page.waitForFunction(() => window.__nfsu2 && !document.querySelector('#play').disabled, null, { timeout: 180000 });
await cdp.send('Storage.overrideQuotaForOrigin', { origin: base, quotaSize: 1024 });
await page.click('#play');
await page.waitForSelector('#confirm[open]', { timeout: 30000 });
const question = await page.textContent('#confirm-title');
await page.click('#confirm-cancel');                       // Not now: settings.ini written, and refused
await page.waitForTimeout(4000);
const st = await page.evaluate(() => ({ stage: !document.querySelector('#stage').hidden, inert: document.querySelector('#app').inert,
  session: !!window.__nfsu2.session }));
await cdp.send('Storage.overrideQuotaForOrigin', { origin: base });
await browser.close();
console.log(`question: ${question}; 4 s after the answer: ${JSON.stringify(st)}; page errors: ${JSON.stringify(errors)}`);
const ok = !st.stage && !st.inert;
console.log(ok ? 'PASS: the start page is back' : 'FAIL: the stage stays up over an inert page, with no game');
process.exitCode = ok ? 0 : 1;
