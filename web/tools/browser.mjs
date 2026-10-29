// The browser the web tools and tests drive: Google Chrome when it is installed (Playwright's channel 'chrome', as
// they always did), else Playwright's own Chromium (npx playwright install chromium; a headless run takes its
// headless shell).  NFSU2_BROWSER=chrome or chromium forces either.  The first pick says on stderr which one it took.
//   chromium.launch({ ...pickBrowser(), headless: true, args })
//   chromium.launchPersistentContext(dir, { ...pickBrowser(), headless: true, args })
import fs from 'node:fs';
import path from 'node:path';

/** Google Chrome's executable where Playwright's channel 'chrome' looks for it (playwright-core's registry), or null */
export function chromeExe() {
  const at = { linux: '/opt/google/chrome/chrome', darwin: '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    win32: '\\Google\\Chrome\\Application\\chrome.exe' }[process.platform];
  if (!at) return null;
  const e = process.env;
  const prefixes = process.platform !== 'win32' ? [''] : [e.LOCALAPPDATA, e.PROGRAMFILES, e['PROGRAMFILES(X86)'],
    e.HOMEDRIVE && `${e.HOMEDRIVE}\\Program Files`, e.HOMEDRIVE && `${e.HOMEDRIVE}\\Program Files (x86)`].filter(Boolean);
  for (const p of prefixes) {
    const exe = path.join(p, at);
    try { fs.accessSync(exe); return exe; } catch { /* not there */ }
  }
  return null;
}

let picked = null;
/** the launch option that picks the browser: { channel: 'chrome' } for Google Chrome, {} for Playwright's Chromium;
 *  the same for every launch of the process */
export function pickBrowser() {
  if (!picked) {
    const force = (process.env.NFSU2_BROWSER || '').toLowerCase();
    if (force && force !== 'chrome' && force !== 'chromium') throw new Error(`NFSU2_BROWSER=${force}: chrome or chromium`);
    const exe = force === 'chromium' ? null : chromeExe();
    picked = force === 'chrome' || exe ? { channel: 'chrome' } : {};
    console.error(picked.channel ? `browser: Google Chrome (${exe || 'not found where Playwright looks'}${force ? ', NFSU2_BROWSER=chrome' : ''})`
      : `browser: Playwright's Chromium (${force ? 'NFSU2_BROWSER=chromium' : 'no Google Chrome installed'})`);
  }
  return { ...picked };
}
