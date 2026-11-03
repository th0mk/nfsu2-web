// How the log names the browser and the device (web/src/app/devicelog.ts), from real user agents:
//   node --test web/test/devicelog.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/devicelog.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { browserBrief, deviceBrief } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const djs = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/device.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { phoneScreen, deviceDefaults } = await import('data:text/javascript;base64,' + Buffer.from(djs).toString('base64'));

test('iPhone and iPad are named as such, not macOS (their user agents say "like Mac OS X" or Macintosh)', () => {
  assert.equal(browserBrief('Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1', 5), 'Safari 26 on iOS');
  assert.equal(browserBrief('Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15', 5), 'Safari 26 on iPadOS');
  assert.equal(browserBrief('Mozilla/5.0 (iPad; CPU OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Mobile/15E148 Safari/604.1', 5), 'Safari 18 on iPadOS');
  assert.equal(browserBrief('Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/140.0.7339.122 Mobile/15E148 Safari/604.1', 5), 'Chrome 140 (WebKit) on iOS');
  assert.equal(browserBrief('Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) FxiOS/142.0 Mobile/15E148 Safari/605.1.15', 5), 'Firefox 142 (WebKit) on iOS');
});

test('an iPhone whose Safari asks for desktop websites: Macintosh in its user agent, a phone by its screen', () => {
  // 2026-10-25, a player's log: "Safari 26 on iPadOS" on a 390x844 screen, and no phone defaults
  const ua = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15';
  assert.equal(phoneScreen(5, 390, 844), true);
  assert.equal(phoneScreen(5, 844, 390), true);                  // turned
  assert.equal(phoneScreen(5, 744, 1133), false);                // iPad mini
  assert.equal(phoneScreen(0, 390, 844), false);                 // no touch: a small window of a desktop
  assert.equal(phoneScreen(10, 1280, 720), false);               // a touch laptop
  assert.equal(browserBrief(ua, 5, phoneScreen(5, 390, 844)), 'Safari 26 on iOS (desktop website)');
  assert.equal(browserBrief(ua, 5, phoneScreen(5, 820, 1180)), 'Safari 26 on iPadOS');
});

test('the desktops as before, Safari on a Mac by name', () => {
  assert.equal(browserBrief('Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.0.1 Safari/605.1.15', 0), 'Safari 27 on macOS');
  assert.equal(browserBrief('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36'), 'Chrome 155 on Windows');
  assert.equal(browserBrief('Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36'), 'Chrome 155 on macOS');
  assert.equal(browserBrief('Mozilla/5.0 (X11; Linux x86_64; rv:145.0) Gecko/20100101 Firefox/145.0'), 'Firefox 145 on Linux');
  assert.equal(browserBrief('Mozilla/5.0 (Linux; Android 15; Pixel 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Mobile Safari/537.36'), 'Chrome 150 on Android');
  // Chrome's user agent says "Android 10; K" on every Android: the version comes from its client hints (androidMajor),
  // the major number only; never on another system, never more than a number
  const reduced = 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Mobile Safari/537.36';
  assert.equal(browserBrief(reduced, 5, true, '11'), 'Chrome 156 on Android 11');
  assert.equal(browserBrief(reduced, 5, true, undefined), 'Chrome 156 on Android');
  assert.equal(browserBrief(reduced, 5, true, '11; vivo 1935'), 'Chrome 156 on Android');
  assert.equal(browserBrief('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36', 0, false, '11'), 'Chrome 155 on Windows');
});

test('the device line: screen, cores, touch, Home Screen, a 30 Hz display before Play', () => {
  assert.equal(deviceBrief({ screenW: 428, screenH: 926, dpr: 3, cores: 6, touchPoints: 5, homeScreen: false, displayHz: 60 }),
    'screen 428x926 at 3x, 6 cores, touch, browser tab');
  assert.equal(deviceBrief({ screenW: 1512, screenH: 982, dpr: 2, cores: 14, memoryGB: 8, touchPoints: 0, homeScreen: false, displayHz: 120 }),
    'screen 1512x982 at 2x, 14 cores, 8 GB memory, no touch, browser tab');
  assert.match(deviceBrief({ screenW: 390, screenH: 844, dpr: 3, touchPoints: 5, homeScreen: true, displayHz: 30 }),
    /^screen 390x844 at 3x, touch, Home Screen app, the display at 30 Hz before Play \(Low Power Mode/);
});

test("free roam's players' cars shown: 16 on a phone, 24 on a tablet, the file's 32 on every computer", () => {
  // 2026-10-25, an iPhone's kept log: stopped for memory as 22 friends' cars loaded at once at the new career's start
  const on = (ua, touchPoints, w, h, coarse, cores = 4, deviceMemory = undefined) => {
    Object.defineProperty(globalThis, 'navigator', { value: { userAgent: ua, maxTouchPoints: touchPoints, hardwareConcurrency: cores, deviceMemory }, configurable: true, writable: true });
    globalThis.screen = { width: w, height: h };
    globalThis.matchMedia = () => ({ matches: coarse });
    return deviceDefaults().roam_max_cars;
  };
  const mac = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15';
  assert.equal(on('Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1', 5, 390, 844, true), '16');
  assert.equal(on(mac, 5, 390, 844, true), '16');                                  // an iPhone asking for desktop websites
  assert.equal(on('Mozilla/5.0 (Linux; Android 15; Pixel 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Mobile Safari/537.36', 5, 412, 915, true), '16');
  assert.equal(on(mac, 5, 820, 1180, true), '24');                                 // an iPad
  assert.equal(on('Mozilla/5.0 (Linux; Android 15; SM-X910) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36', 10, 1848, 2960, true), '24');
  assert.equal(on(mac, 0, 1512, 982, false), undefined);                           // a Mac of 4 cores
  assert.equal(on(mac, 0, 1512, 982, false, 14), undefined);                       // ...of 14: its I/O worker keeps 64 files open
  const firefox = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:145.0) Gecko/20100101 Firefox/145.0';
  assert.equal(on(firefox, 0, 2560, 1440, false, 16), undefined);                  // Firefox on Windows (says no memory)
  const win = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36';
  assert.equal(on(win, 10, 1920, 1080, false), undefined);                          // a touch laptop of 4 cores
  assert.equal(on(win, 0, 1920, 1080, false, 24, 8), undefined);                    // a desktop of 24 cores and 8 GB: 40 and 48 only when picked
  assert.equal(on(win, 0, 1920, 1080, false, 8, 4), undefined);                     // 8 cores, 4 GB
  assert.equal(on(win, 0, 1920, 1080, false, 6, 8), undefined);                     // 6 cores
  assert.equal(on(mac, 5, 820, 1180, true, 8, 8), '24', 'a tablet stays at 24 whatever its cores');
});
