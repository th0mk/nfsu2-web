// Open a page in Playwright-driven Chrome, stream its console, wait for document.title === 'DONE'.
//   node web/tools/run-page.mjs <url> [timeout-ms] [--headed] [--profile <dir>]
// --profile keeps a Chrome user-data directory between runs (localStorage,
// OPFS and Chrome's shader cache persist, as in a real browser).
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
const url = process.argv[2], timeout = +(process.argv[3] || 60000), headed = process.argv.includes('--headed');
const pi = process.argv.indexOf('--profile'), profile = pi > 0 ? process.argv[pi + 1] : null;
const args = ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])];
let browser = null, page;
if (profile) {
  const ctx = await chromium.launchPersistentContext(profile, { ...pickBrowser(), headless: !headed, args });
  page = ctx.pages()[0] ?? await ctx.newPage();
  browser = { close: () => ctx.close() };
} else {
  browser = await chromium.launch({ ...pickBrowser(), headless: !headed, args });
  page = await browser.newPage();
}
page.on('console', m => console.log(m.text())); page.on('pageerror', e => console.log('PAGEERROR ' + e));
await page.goto(url);
try { await page.waitForFunction(() => document.title === 'DONE', null, { timeout }); } catch (e) { console.log('TIMEOUT'); }
await browser.close();
