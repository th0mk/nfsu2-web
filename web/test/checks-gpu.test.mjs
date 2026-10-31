// The start page's graphics check (web/src/app/checks.ts checkGpu) in each browser it tells apart:
//   node --test web/test/checks-gpu.test.mjs
// A fake navigator (user agent, Client Hints, WebGPU) and WebGL per case; each case names the problem's code.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/app/checks.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral' });
const C = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

const UA = {
  chromeWin: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36',
  chromeMac: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36',
  chromeLinux: 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36',
  chromeAndroid: 'Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Mobile Safari/537.36',
  firefoxWin: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0',
  firefoxLinux: 'Mozilla/5.0 (X11; Linux x86_64; rv:158.0) Gecko/20100101 Firefox/158.0',
  firefoxAndroid: 'Mozilla/5.0 (Android 14; Mobile; rv:158.0) Gecko/158.0 Firefox/158.0',
  safariMac: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Safari/605.1.15',
  iphone: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Mobile/15E148 Safari/604.1',
};

/** an adapter with these features and info (Firefox's info is empty) */
const adapter = (features = ['texture-compression-bc'], info = { vendor: 'nvidia', architecture: 'ampere', description: '' }) =>
  ({ features: new Set(features), info });

/**
 * The browser of one case.  gpu: undefined (no navigator.gpu), null (no adapter) or an adapter; webgl: WebGL's
 * renderer string, or null (no WebGL); arch: what Client Hints say the CPU is.
 */
function browser({ ua, gpu, webgl = 'ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Direct3D11 vs_5_0 ps_5_0, D3D11)', arch = 'x86', secure = true, touch = 0 }) {
  const chromium = /Chrome\//.test(ua);
  const nav = {
    userAgent: ua, maxTouchPoints: touch,
    userAgentData: chromium ? {
      brands: [{ brand: 'Google Chrome', version: '155' }], mobile: /Mobile/.test(ua),
      getHighEntropyValues: async () => ({ architecture: arch }),
    } : undefined,
  };
  if (gpu !== undefined) nav.gpu = { requestAdapter: async () => gpu };
  Object.defineProperty(globalThis, 'navigator', { value: nav, configurable: true, writable: true });
  globalThis.isSecureContext = secure;
  globalThis.document = {
    createElement: () => ({
      getContext: () => webgl === null ? null : {
        RENDERER: 1, getExtension: n => n === 'WEBGL_debug_renderer_info' ? { UNMASKED_RENDERER_WEBGL: 2 } : n === 'WEBGL_lose_context' ? { loseContext() {} } : null,
        getParameter: () => webgl,
      },
    }),
  };
}

async function gpuCheck(b) {
  browser(b);
  return C.checkGpu(b.lostBefore);
}

const CASES = [
  ['an http address', { ua: UA.chromeWin, gpu: undefined, secure: false }, 'fail', 'GPU-NOT-SECURE'],
  ['Firefox on Linux, WebGPU off', { ua: UA.firefoxLinux, gpu: undefined }, 'fail', 'GPU-OFF-FIREFOX'],
  ['Firefox for Android', { ua: UA.firefoxAndroid, gpu: undefined }, 'fail', 'GPU-OFF-FIREFOX-ANDROID'],
  ['an old Chrome', { ua: UA.chromeWin, gpu: undefined }, 'fail', 'GPU-OFF'],
  ['an iPhone before iOS 26', { ua: UA.iphone, gpu: undefined }, 'fail', 'GPU-OFF-IOS'],
  ['an iPad (it says Macintosh)', { ua: UA.safariMac, gpu: undefined, touch: 5 }, 'fail', 'GPU-OFF-IOS'],
  ['Safari on a Mac before 26', { ua: UA.safariMac, gpu: undefined }, 'fail', 'GPU-OFF-OTHER'],
  ['Firefox with no adapter', { ua: UA.firefoxWin, gpu: null }, 'fail', 'GPU-ADAPTER-FIREFOX'],
  ['Chrome on an unsupported phone', { ua: UA.chromeAndroid, gpu: null }, 'fail', 'GPU-ADAPTER-ANDROID'],
  ['Chrome without graphics acceleration (no WebGL)', { ua: UA.chromeWin, gpu: null, webgl: null }, 'fail', 'GPU-ACCEL-OFF'],
  ['Chrome without graphics acceleration (SwiftShader)', { ua: UA.chromeLinux, gpu: null,
    webgl: 'ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)' }, 'fail', 'GPU-ACCEL-OFF'],
  ['Chrome on Windows on ARM', { ua: UA.chromeWin, gpu: null, arch: 'arm' }, 'fail', 'GPU-ADAPTER-WIN-ARM'],
  ['Chrome on Windows without Direct3D 12', { ua: UA.chromeWin, gpu: null,
    webgl: 'ANGLE (Intel, Intel(R) HD Graphics 4000 Direct3D11 vs_5_0 ps_5_0, D3D11)' }, 'fail', 'GPU-ADAPTER-WIN'],
  ['Chrome on Linux without a Vulkan driver', { ua: UA.chromeLinux, gpu: null, webgl: 'ANGLE (Intel, Mesa Intel(R) Graphics (RPL-U), OpenGL 4.6)' }, 'fail', 'GPU-ADAPTER-LINUX'],
  ['Chrome on a Mac with no adapter', { ua: UA.chromeMac, gpu: null }, 'fail', 'GPU-ADAPTER'],
  // the last game's graphics went (GPU-LOST): no adapter after it is the browser's doing, not old graphics
  ['Chrome on Windows after the last game lost its graphics', { ua: UA.chromeWin, gpu: null, lostBefore: true,
    webgl: 'ANGLE (Intel, Intel(R) Iris(R) Xe Graphics Direct3D11 vs_5_0 ps_5_0, D3D11)' }, 'fail', 'GPU-ADAPTER-AFTER-LOST'],
  ['Chrome with an adapter after the last game lost its graphics', { ua: UA.chromeWin, gpu: adapter(), lostBefore: true }, 'ok', undefined],
  ['a phone GPU without BC', { ua: UA.chromeAndroid, gpu: adapter([], { vendor: 'arm', architecture: 'valhall', description: '' }) }, 'warn', 'GPU-NO-BC'],
  // DXT1 goes up as ASTC there (web/src/gpu/astc.ts): only ~50 MB more than BC, no note
  ['an iPhone: no BC, but ASTC', { ua: UA.iphone, gpu: adapter(['texture-compression-astc'], { vendor: 'apple', architecture: 'common-3', description: '' }) }, 'ok', undefined],
  ['a phone GPU without BC, with ASTC', { ua: UA.chromeAndroid, gpu: adapter(['texture-compression-astc', 'texture-compression-etc2'], { vendor: 'qualcomm', architecture: 'adreno-7xx', description: '' }) }, 'ok', undefined],
  ['Firefox on Intel integrated graphics', { ua: UA.firefoxWin, gpu: adapter(undefined, {}), webgl: 'Intel(R) HD Graphics 400' }, 'warn', 'GPU-INTEGRATED-FIREFOX'],
  ['Firefox on a graphics card', { ua: UA.firefoxWin, gpu: adapter(undefined, {}), webgl: 'NVIDIA GeForce GTX 980 Direct3D11 vs_5_0 ps_5_0' }, 'ok', undefined],
  ['Chrome on a graphics card', { ua: UA.chromeWin, gpu: adapter() }, 'ok', undefined],
];

for (const [name, b, state, code] of CASES) {
  test(`graphics check: ${name} → ${code ?? state}`, async () => {
    const c = await gpuCheck(b);
    assert.equal(c.state, state, c.detail);
    assert.equal(c.code, code, c.detail);
    assert.doesNotMatch(c.detail, /\{\w+\}/, 'every {name} filled');
  });
}

test('graphics check: the titles and the browser\'s name in the text', async () => {
  let c = await gpuCheck({ ua: UA.chromeWin, gpu: null, webgl: null });
  assert.equal(c.label, 'Graphics (WebGPU)');
  // the text as written (td, the key the page translates) filled with the browser's name, not its wording
  const filled = c => c.td.replace(/\{(\w+)\}/g, (m, k) => c.v[k]);
  c = await gpuCheck({ ua: UA.chromeMac, gpu: null });
  assert.equal(c.code, 'GPU-ADAPTER');
  assert.deepEqual(c.v, { browser: 'Chrome', Browser: 'Chrome' });
  assert.equal(c.detail, filled(c));
  assert.ok(c.detail.includes('Chrome'), c.detail);
  // Safari 26 with WebGPU but no adapter: "the browser", capitalised where it starts the sentence
  c = await gpuCheck({ ua: UA.safariMac, gpu: null });
  assert.deepEqual(c.v, { browser: 'the browser', Browser: 'The browser' });
  assert.equal(c.detail, filled(c));
  assert.equal(c.tl, 'Graphics (WebGPU)');
  c = await gpuCheck({ ua: UA.chromeWin, gpu: adapter() });
  assert.equal(c.label, 'Graphics: nvidia ampere');
  assert.equal(c.tl, 'Graphics: {info}', 'the title stays a text the page translates');
  c = await gpuCheck({ ua: UA.firefoxWin, gpu: adapter(undefined, {}), webgl: 'Intel(R) HD Graphics 400' });
  assert.equal(c.label, 'Graphics: Intel (integrated)');
  c = await gpuCheck({ ua: UA.firefoxWin, gpu: adapter(undefined, {}), webgl: 'Intel(R) Arc(TM) A750 Graphics' });
  assert.equal([c.state, c.label].join(' '), 'ok Graphics: Intel');
});
