// Drives a page with trusted input (Playwright keyboard, mouse, a fake
// gamepad) while it runs, synchronised on its console output, and waits for
// document.title === 'DONE' like run-page.mjs.
//
//   node web/tools/drive.mjs <url> <timeout-ms> <steps.json> [--profile DIR] [--headed]
//        [--done "<js expression>"] [--shots DIR] [--size WxH|window] [--cdp http://localhost:9341]
//
// --done: the run is over when this is true in the page (default document.title
// === 'DONE'); the app's start page: "window.__nfsu2?.phase === 'exited'".
//
// steps.json: an array of steps, run in order:
//   {"wait": "regex", "timeout": ms}          until a console line matches
//   {"press": "Enter", "until": "regex", "every": ms, "timeout": ms}
//                                             press (repeatedly with until)
//   {"down": "KeyW"} / {"up": "KeyW"}         keyboard.down / up (KeyboardEvent.code names)
//   {"sleep": ms}
//   {"mouse": [x, y]} / {"click": [x, y]} / {"wheel": dy}   page coordinates
//   {"pad": {"buttons": {"0": 1, "7": 0.8}, "axes": [lx, ly, rx, ry], "id": "..."}}
//                                             the fake standard gamepad's state
//                                             (buttons not named keep their value)
//   {"padtap": 0, "until": "regex", "every": ms, "timeout": ms}  press and release button i
//   {"padinit": {"id": "...", "mapping": "", "axes": [...], "buttons": n}}
//                                             the fake pad from the start: its Gamepad.id (default Xbox),
//                                             mapping (default "standard"; "" a wheel's), axes at rest and
//                                             number of buttons (a wheel: "pad" steps then give all its axes)
//   {"eval": "js expression"}                 page.evaluate, the result is logged
//   {"waitfor": "js expression", "timeout": ms}   until it is true in the page
//   {"clicksel": "#play"}                      a trusted click on that element
//   {"screenshot": "name.png"}                 the page as the person sees it, into --shots
//   {"activate": true}                         bring the Chrome of --os-pid to the front (macOS)
//   {"oskey": "Escape", "hold": ms}            a real macOS key event (CGEventPost: through the browser's
//                                             own key handling, unlike Playwright's), sent only while the
//                                             frontmost app is the process --os-pid (the Chrome under test,
//                                             never another one); needs the Accessibility permission
// --cdp: drive a Chrome started by hand (--remote-debugging-port) instead of launching one: a
// normal window, not an automation one (full screen covers the screen, Escape reaches Chrome's
// own full-screen handling)
//   {"log": "text"}
// --profile DIR keeps the browser profile (OPFS saves) between runs.
// A fake gamepad is installed only when a step uses one: navigator.getGamepads()
// then returns it (Chrome exposes no controller to headless pages).
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';

const [url, tmo, stepsFile] = process.argv.slice(2);
const timeout = +(tmo || 60000), headed = process.argv.includes('--headed');
const opt = (n) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : null; };
const profile = opt('--profile'), doneExpr = opt('--done') ?? "document.title === 'DONE'", shots = opt('--shots') ?? '.';
// --size window: no viewport emulation, the page gets the real window (headed:
// full screen then covers the real screen at its own pixels and refresh rate)
const [vw, vh] = (opt('--size') ?? '1280x720').split('x').map(Number);
const viewport = opt('--size') === 'window' ? null : { width: vw, height: vh };
const steps = JSON.parse(fs.readFileSync(stepsFile, 'utf8'));
const usesPad = steps.some(s => s.pad || s.padtap !== undefined || s.padinit);
// {"padinit": {...}}: the controller the fake pad claims to be from the start
const padInit = steps.find(s => s.padinit)?.padinit ?? {};
const padSpec = { id: padInit.id ?? 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)',
  mapping: padInit.mapping ?? 'standard', axes: padInit.axes ?? [0, 0, 0, 0], buttons: padInit.buttons ?? 17 };
const args = ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])];

const cdp = opt('--cdp'), osPid = opt('--os-pid');
const ctx = cdp ? (await chromium.connectOverCDP(cdp)).contexts()[0]
  : profile
  ? await chromium.launchPersistentContext(profile, { ...pickBrowser(), headless: !headed, args, viewport, bypassCSP: true })
  : await (await chromium.launch({ ...pickBrowser(), headless: !headed, args })).newContext({ viewport, bypassCSP: true });   // string waitForFunction needs eval; the server's CSP forbids it
const page = ctx.pages()[0] ?? await ctx.newPage();
if (usesPad) await page.addInitScript((spec) => {
  // a controller (standard mapping unless padinit says otherwise) whose state the driver sets
  const pad = { id: spec.id, index: 0, connected: true,
    mapping: spec.mapping, timestamp: 0, axes: spec.axes.slice(), vibrationActuator: null,
    buttons: Array.from({ length: spec.buttons }, () => ({ pressed: false, touched: false, value: 0 })) };
  window.__fakePad = pad;
  window.__setPad = (s) => {
    if (s.id) pad.id = s.id;
    for (const [i, v] of Object.entries(s.buttons ?? {})) pad.buttons[+i] = { pressed: v > 0.5, touched: v > 0, value: v };
    if (s.axes) pad.axes = s.axes.slice();
    pad.timestamp = performance.now();
  };
  navigator.getGamepads = () => [pad, null, null, null];
}, padSpec);

const lines = [];
let waiters = [];
page.on('console', m => {
  const t = m.text(); console.log(t); lines.push(t);
  waiters = waiters.filter(w => { if (w.re.test(t)) { w.res(t); return false; } return true; });
});
page.on('pageerror', e => console.log('PAGEERROR ' + e));
const waitLine = (re, ms) => new Promise((res, rej) => {
  const w = { re, res }; waiters.push(w);
  setTimeout(() => { waiters = waiters.filter(x => x !== w); rej(new Error(`no line /${re.source}/ within ${ms} ms`)); }, ms);
});
const sleep = ms => new Promise(r => setTimeout(r, ms));
// a JS expression as a function made here: Playwright evaluates a string predicate
// with eval inside the page, which the player's page forbids (CSP script-src without
// 'unsafe-eval', web/server-rs/crates/headers); a function goes over DevTools instead
const predicate = expr => new Function(`return (${expr});`);
const seen = (re, from) => lines.slice(from).some(l => re.test(l));

await page.goto(url);
const done = page.waitForFunction(predicate(doneExpr), null, { timeout, polling: 250 }).then(() => true, () => false);
let failed = false;
const stepsDone = (async () => {
  for (const s of steps) {
    try {
      if (s.log) console.log(`[drive] ${s.log}`);
      if (s.wait) { await waitLine(new RegExp(s.wait), s.timeout ?? timeout); console.log(`[drive] seen /${s.wait}/`); }
      if (s.sleep) await sleep(s.sleep);
      if (s.down) await page.keyboard.down(s.down);
      if (s.up) await page.keyboard.up(s.up);
      if (s.mouse) await page.mouse.move(s.mouse[0], s.mouse[1]);
      if (s.click) await page.mouse.click(s.click[0], s.click[1]);
      if (s.wheel) await page.mouse.wheel(0, s.wheel);
      if (s.pad) await page.evaluate(st => window.__setPad(st), s.pad);
      if (s.eval) console.log(`[drive] eval ${s.eval} -> ${JSON.stringify(await page.evaluate(s.eval))}`);
      if (s.waitfor) { await page.waitForFunction(predicate(s.waitfor), null, { timeout: s.timeout ?? timeout, polling: 100 }); console.log(`[drive] true: ${s.waitfor}`); }
      if (s.clicksel) { await page.click(s.clicksel); console.log(`[drive] clicked ${s.clicksel}`); }
      if (s.activate) {
        if (!osPid) throw new Error('activate needs --os-pid');
        const { execFileSync } = await import('node:child_process');
        execFileSync('osascript', ['-l', 'JavaScript', '-e', `ObjC.import('AppKit'); $.NSRunningApplication.runningApplicationWithProcessIdentifier(${+osPid}).activateWithOptions($.NSApplicationActivateAllWindows)`]);
        await sleep(500);
      }
      if (s.oskey) {
        const { execFileSync } = await import('node:child_process');
        const front = /pid = (\d+)/.exec(execFileSync('sh', ['-c', 'lsappinfo info -only pid "$(lsappinfo front)"'], { encoding: 'utf8' }))?.[1];
        if (!osPid || front !== String(osPid)) throw new Error(`not sending a real key: the frontmost process is ${front}, not the Chrome under test (--os-pid ${osPid})`);
        const code = { Escape: 53, Enter: 36, Space: 49 }[s.oskey];
        execFileSync('osascript', ['-l', 'JavaScript', '-e', `ObjC.import('ApplicationServices');
          $.CGEventPost($.kCGHIDEventTap, $.CGEventCreateKeyboardEvent(null, ${code}, true)); delay(${(s.hold ?? 80) / 1000});
          $.CGEventPost($.kCGHIDEventTap, $.CGEventCreateKeyboardEvent(null, ${code}, false));`]);
        console.log(`[drive] real key ${s.oskey} held ${s.hold ?? 80} ms`);
      }
      if (s.screenshot) { fs.mkdirSync(shots, { recursive: true }); await page.screenshot({ path: `${shots}/${s.screenshot}` }); console.log(`[drive] screenshot ${shots}/${s.screenshot}`); }
      if (s.press || s.padtap !== undefined) {
        const re = s.until ? new RegExp(s.until) : null, t0 = Date.now(), from = lines.length;
        for (let n = 0; ; n++) {
          if (s.press) await page.keyboard.press(s.press, { delay: 50 });
          else { await page.evaluate(i => window.__setPad({ buttons: { [i]: 1 } }), s.padtap); await sleep(80);
                 await page.evaluate(i => window.__setPad({ buttons: { [i]: 0 } }), s.padtap); }
          if (!re) break;
          const end = Date.now() + (s.every ?? 1500);
          while (Date.now() < end && !seen(re, from)) await sleep(50);
          if (seen(re, from)) { console.log(`[drive] ${s.press ?? 'pad ' + s.padtap} x${n + 1} until /${s.until}/: seen`); break; }
          if (Date.now() - t0 > (s.timeout ?? 30000)) throw new Error(`/${s.until}/ not seen pressing ${s.press ?? s.padtap}`);
        }
      }
    } catch (e) { console.log(`[drive] FAILED at ${JSON.stringify(s)}: ${e.message}`); failed = true; break; }
  }
  console.log(`[drive] steps ${failed ? 'FAILED' : 'done'}`);
})();
const ok = await done;
if (!ok) console.log('TIMEOUT');
// steps after the end (a last screenshot or eval) still run, briefly
else await Promise.race([stepsDone, sleep(15000)]);
await ctx.close();
process.exit(ok && !failed ? 0 : 1);
