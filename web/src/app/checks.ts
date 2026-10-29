// What this browser and this page need before the game can start, each with
// a message that says what to do when it is missing.  The start page shows
// only the ones that fail or warn, in the page's language (i18n.ts), each
// problem with its code: the same in every language, so a screenshot says
// which one it is.

import { homeScreenInstead } from './display';
import { fill, N, type Vars } from './i18n';

export type CheckState = 'pending' | 'ok' | 'warn' | 'fail';
/**
 * label and detail in English, filled in (the log, the tests); tl and td: the same as written, with {name}s for v,
 * which the page shows in its language; code: a failure's or warning's fixed name (GPU-ADAPTER-WIN)
 */
export interface Check { id: string; label: string; state: CheckState; detail: string; code?: string; tl?: string; td?: string; v?: Vars }

/** a check whose label and detail are English with {name}s for v */
export function check(id: string, label: string, state: CheckState, detail: string, code?: string, v?: Vars): Check {
  return { id, label: fill(label, v), state, detail: fill(detail, v), code, tl: label, td: detail, v };
}

export const MIN_CHROME = 140;
/** Atomics.waitAsync (the renderer's and the network's doorbells) is in Firefox from 145 */
export const MIN_FIREFOX = 145;

/** "Google Chrome" major version, or null (another browser), from User-Agent Client Hints. */
export function chromeVersion(): { brand: string; major: number } | null {
  const uad = (navigator as any).userAgentData as { brands?: { brand: string; version: string }[] } | undefined;
  const b = uad?.brands?.find(x => x.brand === 'Google Chrome') ?? uad?.brands?.find(x => x.brand === 'Chromium');
  if (b) return { brand: b.brand, major: parseInt(b.version, 10) };
  const m = /Chrome\/(\d+)/.exec(navigator.userAgent);
  return m ? { brand: 'Chromium', major: +m[1] } : null;
}

/** Firefox's major version, or null (another browser; Firefox on iOS is WebKit and says FxiOS). */
export function firefoxVersion(): number | null {
  const m = /Firefox\/(\d+)/.exec(navigator.userAgent);
  return m ? +m[1] : null;
}

/** what the messages call this browser: Firefox, Chrome (any Chromium browser) or the browser */
export function browserName(): string {
  return firefoxVersion() !== null ? 'Firefox' : chromeVersion() ? 'Chrome' : N('the browser');
}

/**
 * Which browser this is: a warning at most, never a reason to stop.  What the
 * game needs is checked by what it is (WebGPU, isolation, storage below), so
 * another browser that has all of it may play.
 */
export function checkBrowser(): Check {
  const f = firefoxVersion();
  if (f !== null) {
    if (f < MIN_FIREFOX) return check('browser', 'Firefox {v}', 'warn', N('Only the latest Firefox is tested ({min} or newer). Update Firefox (Help, About Firefox) if something does not work.'), 'BROWSER-FIREFOX-OLD', { v: f, min: MIN_FIREFOX });
    return check('browser', 'Firefox {v}', 'ok', N('A tested browser.'), undefined, { v: f });
  }
  const c = chromeVersion();
  const other = N('Only Google Chrome and Firefox are tested. This browser may work if every other check passes.');
  if (homeScreenInstead()) return check('browser', N('Browser'), 'warn', N('Only Google Chrome and Firefox are tested. On an iPhone a web page cannot go full screen: to play without the browser\'s bars, add this page to the Home Screen (in the Share menu) and start the game from its icon. That copy keeps its own saves: a passphrase (Cloud saves) brings yours over.'), 'BROWSER-IPHONE');
  if (!c) return check('browser', N('Browser'), 'warn', other, 'BROWSER-OTHER');
  if (c.major < MIN_CHROME) return check('browser', 'Chrome {v}', 'warn', N('Only the latest Chrome is tested ({min} or newer). Update Chrome (Help, About Google Chrome) if something does not work.'), 'BROWSER-CHROME-OLD', { v: c.major, min: MIN_CHROME });
  if (c.brand !== 'Google Chrome') return check('browser', '{brand} {v}', 'warn', other, 'BROWSER-CHROMIUM', { brand: c.brand, v: c.major });
  return check('browser', 'Chrome {v}', 'ok', N('A tested browser.'), undefined, { v: c.major });
}

export function checkIsolation(): Check {
  const label = N('Secure, isolated page');
  if (!isSecureContext) {
    return check('isolation', label, 'fail', N('This address is not secure, so {browser} turns off what the game needs. Open the server\'s https:// address instead.'),
      'PAGE-NOT-SECURE', { browser: browserName() });
  }
  if (!crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
    return check('isolation', label, 'fail', N('The page is not cross-origin isolated (no SharedArrayBuffer). Open it from the NFSU2 web server, not another server.'), 'PAGE-NOT-ISOLATED');
  }
  try { new WebAssembly.Memory({ initial: 1, maximum: 65536, shared: true }); }
  catch (e) { return check('isolation', label, 'fail', N('Shared 4 GB WebAssembly memory is not available: {error}'), 'PAGE-NO-MEMORY', { error: String(e) }); }
  if (typeof (Atomics as { waitAsync?: unknown }).waitAsync !== 'function') {
    return check('isolation', label, 'fail', N('This browser is too old for the game (no Atomics.waitAsync). Update {browser}.'), 'PAGE-NO-WAITASYNC', { browser: browserName() });
  }
  return check('isolation', label, 'ok', N('HTTPS or localhost, cross-origin isolated, threads and 4 GB memory available.'));
}

/**
 * The graphics, in words a player follows: what is wrong and what to do about it, with the
 * browser's own setting or address to type where there is one.  Chrome's own list of causes
 * (developer.chrome.com/docs/web-platform/webgpu/troubleshooting-tips) and where each browser
 * has WebGPU on (github.com/gpuweb/gpuweb/wiki/Implementation-Status) decide the cases.
 * lostBefore: the last game stopped because its graphics went (GPU-LOST).
 */
export async function checkGpu(lostBefore = false): Promise<Check & { adapter?: string }> {
  const label = N('Graphics (WebGPU)');
  const gpu = (navigator as any).gpu as GPU | undefined;
  const ua = navigator.userAgent;
  const ff = firefoxVersion() !== null, chromium = !ff && chromeVersion() !== null;
  const android = /Android/.test(ua), linux = /Linux/.test(ua) && !android, windows = /Windows/.test(ua);
  // every browser on an iPhone or iPad is Safari's engine (an iPad says Macintosh, with touch)
  const ios = /iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1);
  const fail = (detail: string, code: string, v?: Vars) => check('gpu', label, 'fail', detail, code, v);
  // navigator.gpu is there in secure pages only: the isolation check says what to do
  if (!isSecureContext) return fail(N('Off because this address is not secure (see above).'), 'GPU-NOT-SECURE');
  // Firefox: on by itself on Windows (141) and on Macs with Apple chips (147), elsewhere in Nightly only
  if (!gpu) return ff
    ? android
      ? fail(N('Firefox for Android has no WebGPU yet, which the game needs. Play in Chrome.'), 'GPU-OFF-FIREFOX-ANDROID')
      : fail(N('This Firefox has no WebGPU, which the game needs. On Windows and on Macs with Apple chips, update Firefox (Help, About Firefox). Elsewhere, such as on Linux, play in Chrome, or turn WebGPU on yourself: in about:config, set dom.webgpu.enabled to true.'), 'GPU-OFF-FIREFOX')
    : chromium
      ? fail(N('This Chrome has no WebGPU, which the game needs. Update Chrome (Help, About Google Chrome). On Linux, also turn on chrome://flags/#enable-unsafe-webgpu, then restart Chrome.'), 'GPU-OFF')
      : ios
        ? fail(N('This browser has no WebGPU, which the game needs. It comes with iOS 26 and iPadOS 26: update this device (Settings, General, Software Update).'), 'GPU-OFF-IOS')
        : fail(N('This browser has no WebGPU, which the game needs. Play in Chrome or Firefox.'), 'GPU-OFF-OTHER');
  let ad: GPUAdapter | null = null;
  try { ad = await gpu.requestAdapter({ powerPreference: 'high-performance' }); } catch { /* reported below */ }
  if (!ad) {
    // the graphics went during the last game, and the reloaded page gets no adapter: the browser keeps them off for a
    // while, not graphics too old (two players' Intel Iris Xe, a hung Direct3D 12 device, then GPU-ADAPTER-WIN in Chrome
    // and Brave).  Chrome blocks a site whose graphics hung from them for two minutes (gpu_data_manager_impl_private.cc,
    // kBlockedDomainExpirationPeriod); the page asks again until they are back (shell.ts recheckGpu)
    if (lostBefore) {
      const b = browserName();
      return fail(N('The graphics stopped during your last game, and {browser} has not given them back to this page yet. The page checks again every 10 seconds: wait a minute or two. If Play stays off, close {browser} completely (every window) and open it again. If the graphics stop again, update the graphics driver (from Intel, AMD or NVIDIA).'), 'GPU-ADAPTER-AFTER-LOST', { browser: b });
    }
    // Firefox has one list of graphics drivers it leaves out, and on Windows, as Chrome, Direct3D 12 only
    if (ff) return fail(N('Firefox will not use this computer\'s graphics for the game. The graphics or their driver may be too old: update the graphics driver, or try Chrome. about:support, Graphics, says why.'), 'GPU-ADAPTER-FIREFOX');
    if (chromium) {
      // Chrome 121: Android 12 and newer, on some graphics chips; the unsafe flag lifts its blocklist (Samsung's
      // Xclipse runs the game with it)
      if (android) return fail(N('Chrome cannot use the graphics of this phone or tablet yet: it needs Android 12 or newer, and not every graphics chip is supported. Update Android and Chrome. If that does not help, turn on chrome://flags/#enable-unsafe-webgpu (type it into the address bar), then restart Chrome: on some phones, such as Samsungs with Xclipse graphics, the game then runs.'), 'GPU-ADAPTER-ANDROID');
      // graphics acceleration off in Chrome's settings (or graphics it uses for nothing): WebGL draws in software, or not at all
      const gl = webglRenderer();
      if (gl === null || /SwiftShader|llvmpipe|softpipe|Basic Render/i.test(gl))
        return fail(N('Chrome is not using this computer\'s graphics at all. Open chrome://settings/system, turn on graphics acceleration, then restart Chrome. If it is on already, update the graphics driver.'), 'GPU-ACCEL-OFF');
      // Windows on ARM (Snapdragon): WebGPU behind a flag in Chrome
      if (windows && await architecture() === 'arm')
        return fail(N('On Windows computers with an ARM chip (Snapdragon, for one), Chrome keeps WebGPU off. Turn on chrome://flags/#enable-unsafe-webgpu, then restart Chrome.'), 'GPU-ADAPTER-WIN-ARM');
      // Chrome on Linux runs WebGPU on Vulkan while the rest of it stays on OpenGL, so
      // chrome://gpu says WebGPU is on even when no Vulkan driver is installed
      if (linux) return fail(N('Chrome could not use this computer\'s graphics. Most likely their Vulkan driver is missing: install it (the package mesa-vulkan-drivers on Debian, Ubuntu and Fedora), then restart Chrome. If chrome://gpu says WebGPU is disabled, also turn on chrome://flags/#enable-unsafe-webgpu.'), 'GPU-ADAPTER-LINUX');
      // and on Windows on Direct3D 12 only: the D3D11 adapters are compatibility mode, blocklisted
      if (windows) return fail(N('Chrome could not use this computer\'s graphics. On Windows it needs graphics with DirectX 12, which older ones (Intel HD 4000 and older, for one) do not have: the game cannot run on them. On newer graphics, update the graphics driver (from Intel, AMD or NVIDIA), then restart Chrome.'), 'GPU-ADAPTER-WIN');
    }
    // macOS, ChromeOS, another browser: a crashed graphics process (a restart), or an old system
    const b = browserName();
    return fail(N('{Browser} could not use this computer\'s graphics for the game. Restart {browser}; if that does not help, update {browser} and the system.'), 'GPU-ADAPTER',
      { browser: b, Browser: b === 'the browser' ? N('The browser') : b });
  }
  let info = ad.info ? [ad.info.vendor, ad.info.architecture, ad.info.description].filter(Boolean).join(' ') : '';
  const ffg = ff && windows && !info ? firefoxGraphics() : null;
  if (ffg) info = ffg.name;
  const named = info ? N('Graphics: {info}') : label;
  // no BC (PowerVR, Mali): the renderer unpacks the textures (web/src/gpu/dxt.ts), so it plays.  With ASTC (iPhones,
  // iPads, most phones) the most of them go up as ASTC instead (astc.ts): ~50 MB more, not 200, nothing to say
  if (!ad.features.has('texture-compression-bc') && !ad.features.has('texture-compression-astc')) {
    return { ...check('gpu', named, 'warn', N('This device\'s graphics cannot read the game\'s packed textures, so the page unpacks them while the game loads. The game plays as it should, but it needs about 200 MB more memory, and loading screens may pause for a moment.'), 'GPU-NO-BC', { info }), adapter: info };
  }
  if (ffg?.integrated) {
    return { ...check('gpu', named, 'warn', N('Firefox runs the game on the graphics built into the processor. If this computer also has a graphics card (NVIDIA or AMD), the game runs much better on it: in Windows Settings, System, Display, Graphics, set Firefox to High performance, then restart Firefox.'), 'GPU-INTEGRATED-FIREFOX', { info }), adapter: info };
  }
  return { ...check('gpu', named, 'ok', N('Adapter with BC texture compression.'), undefined, { info }), adapter: info };
}

/** WebGL's renderer string (the unmasked one where the browser gives it), or null: no WebGL here */
function webglRenderer(): string | null {
  try {
    const gl = document.createElement('canvas').getContext('webgl');
    if (!gl) return null;
    const ext = gl.getExtension('WEBGL_debug_renderer_info');
    const r = String(gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER));
    gl.getExtension('WEBGL_lose_context')?.loseContext();
    return r;
  } catch { return null; }
}

/** the computer's CPU architecture ('arm', 'x86') from Chromium's User-Agent Client Hints, or null */
async function architecture(): Promise<string | null> {
  try { return (await (navigator as any).userAgentData?.getHighEntropyValues?.(['architecture']))?.architecture ?? null; }
  catch { return null; }
}

/**
 * Firefox on Windows: which graphics the game runs on.  Firefox gives WebGPU the adapter its
 * compositor draws the windows with, whatever powerPreference asks (gfx/wgpu_bindings server.rs:
 * the D3D12 adapter with the compositor's LUID), and its adapter.info is empty.  WebGL's renderer
 * string names the same graphics as a vendor and a class (dom/canvas/SanitizeRenderer.cpp): Intel's
 * integrated graphics become "Intel(R) HD Graphics[ 400]" (Arc cards "Intel(R) Arc(TM) A750
 * Graphics"), and the Ryzen APUs' unnumbered "AMD Radeon(TM) Graphics" "Radeon HD 3200 Graphics".
 * (Chrome does the same: powerPreference has no effect there either, per Chrome's troubleshooting
 * page, but its adapter.info names the graphics.)
 */
function firefoxGraphics(): { name: string; integrated: boolean } | null {
  const r = webglRenderer();
  if (r === null) return null;
  const vendor = /NVIDIA|GeForce|Quadro/.test(r) ? 'NVIDIA' : /Intel/.test(r) ? 'Intel' : /AMD|Radeon/.test(r) ? 'AMD' : null;
  if (!vendor) return null;
  const integrated = vendor === 'Intel' ? !/Arc/.test(r) : vendor === 'AMD' && /Radeon HD 3200/.test(r);
  return { name: `${vendor}${integrated ? ' (integrated)' : ''}`, integrated };
}

/**
 * Firefox asks the player before it keeps a site's data for good (a prompt by
 * the address bar), and persist() waits for the answer: the check does not.
 * It says so then, and `answer` is the check again once the player answered.
 * Without that promise the data stays until the disk is nearly full, which the
 * page cannot see: Chrome's estimate() tells such a site usage + 10 GiB (since
 * Chrome 133), whatever the disk has.  So that is no warning: backups are the
 * safety net (docs/web/PLAYING.md).
 */
export async function checkStorage(): Promise<Check & { answer?: Promise<Check> }> {
  const label = N('Browser storage');
  try { await navigator.storage.getDirectory(); }
  catch { return check('storage', label, 'fail', N('This browser window cannot store files (a guest or private window?). Saves need the origin private file system.'), 'STORAGE-NONE'); }
  const verdict = (persisted: boolean): Check => persisted
    ? check('storage', label, 'ok', N('Saves and cached game data are kept for good.'))
    : check('storage', label, 'ok', N('Saves and cached game data are kept until the disk is nearly full, when {browser} may clear them.'), undefined, { browser: browserName() });
  const ask = (async () => await navigator.storage.persisted() || await navigator.storage.persist())()
    .catch(() => false);                 // not decisive
  const quick = await Promise.race([ask, new Promise<null>(res => setTimeout(() => res(null), 500))]);
  if (quick !== null) return verdict(quick);
  const b = browserName();
  return { ...check('storage', label, 'warn', N('{Browser} asks, by the address bar, whether this site may keep its data for good: allow it, so that your saves stay when the disk runs low.'),
    'STORAGE-ASK', { Browser: b === 'the browser' ? N('The browser') : b }), answer: ask.then(verdict) };
}

export function fmtBytes(n: number): string {
  if (n >= 1e9) return `${(n / 1e9).toFixed(1)} GB`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(n >= 1e8 ? 0 : 1)} MB`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(0)} KB`;
  return `${n} bytes`;
}
