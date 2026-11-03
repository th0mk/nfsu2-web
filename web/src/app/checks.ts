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

/** The JavaScript optimizer is off for this site: Chrome's site setting (since 133), Android's Advanced Protection
 *  (Chrome 137 on Android 16) or Edge's Enhanced security.  Chrome then has no WebAssembly at all (checkIsolation), or
 *  only its interpreter, which can't compile the game: "exnref not supported by the Wasm interpreter" (shell.ts). */
export const NO_JIT = N('The JavaScript optimizer is off for this site, and the game can\'t run without it. In Chrome: Settings, Privacy and security, JavaScript optimization and security, allow this site (Android\'s Advanced Protection turns it off). In Edge: add this site as an exception to Enhanced security.');
/** a game module that did not compile for that reason */
export const noJit = (e: unknown) => /Wasm interpreter|WebAssembly is not defined/i.test(String(e));

/**
 * A Chromium browser's name (Google Chrome, Microsoft Edge, Brave, Opera; Chromium where it names itself not), its
 * own major version (Opera's and Samsung Internet's aren't Chromium's) and Chromium's, or null (another browser),
 * from User-Agent Client Hints.  Their lists hold a made-up "Not?A_Brand" too.
 */
export function chromeVersion(): { brand: string; own: number; major: number } | null {
  const uad = (navigator as any).userAgentData as { brands?: { brand: string; version: string }[] } | undefined;
  const all = uad?.brands ?? [];
  const named = all.find(x => !/^Chromium$|Not.A.Brand/i.test(x.brand)), engine = all.find(x => x.brand === 'Chromium') ?? named;
  if (engine) {
    const b = named ?? engine;
    return { brand: b.brand, own: parseInt(b.version, 10), major: parseInt(engine.version, 10) };
  }
  const m = /Chrome\/(\d+)/.exec(navigator.userAgent);
  return m ? { brand: 'Chromium', own: +m[1], major: +m[1] } : null;
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

/** Safari's major version, 0 for the other browsers on an iPhone or iPad (all Safari's engine, no Version/), or null */
export function safariVersion(): number | null {
  const ua = navigator.userAgent;
  if (!/AppleWebKit\//.test(ua) || /Chrome\/|Firefox\//.test(ua)) return null;     // Chromium says AppleWebKit too
  const m = /Version\/(\d+)/.exec(ua);
  return m ? +m[1] : 0;
}

/**
 * Which browser this is: a warning at most, never a reason to stop.  What the
 * game needs is checked by what it is (WebGPU, isolation, storage below), so
 * Edge, Brave, Opera (Chrome's engine) and Safari get no warning: the checks
 * below say what one lacks.
 */
export function checkBrowser(): Check {
  const f = firefoxVersion();
  if (f !== null) {
    if (f < MIN_FIREFOX) return check('browser', 'Firefox {v}', 'warn', N('Tested on Firefox {min} or newer: update it (Help, About Firefox) if something doesn\'t work.'), 'BROWSER-FIREFOX-OLD', { v: f, min: MIN_FIREFOX });
    return check('browser', 'Firefox {v}', 'ok', N('A tested browser.'), undefined, { v: f });
  }
  if (homeScreenInstead()) return check('browser', N('Browser'), 'warn', N('For full screen on iPhone, add this page to the Home Screen (Share menu) and play from its icon. That copy has its own saves: Cloud saves bring yours over.'), 'BROWSER-IPHONE');
  const c = chromeVersion();
  if (c) {
    const brand = c.brand.replace(/^(Google|Microsoft) /, '');
    if (c.major < MIN_CHROME) return check('browser', '{brand} {v}', 'warn', N('Tested on Chrome {min} or newer: update it (Help, About Google Chrome) if something doesn\'t work.'), 'BROWSER-CHROME-OLD', { brand, v: c.own, min: MIN_CHROME });
    return check('browser', '{brand} {v}', 'ok', N('A tested browser.'), undefined, { brand, v: c.own });
  }
  const s = safariVersion();
  if (s) return check('browser', 'Safari {v}', 'ok', N('A tested browser.'), undefined, { v: s });
  if (s === 0) return check('browser', N('Browser'), 'ok', N('A tested browser.'));
  return check('browser', N('Browser'), 'warn', N('Only Chrome and Firefox are tested. This browser may still work.'), 'BROWSER-OTHER');
}

export function checkIsolation(): Check {
  const label = N('Secure, isolated page');
  if (!isSecureContext) {
    return check('isolation', label, 'fail', N('This address is not secure, so {browser} blocks what the game needs. Use the https:// address.'),
      'PAGE-NOT-SECURE', { browser: browserName() });
  }
  if (!crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
    return check('isolation', label, 'fail', N('The page is not cross-origin isolated (no SharedArrayBuffer). Open it from the NFSU2 web server.'), 'PAGE-NOT-ISOLATED');
  }
  // the JavaScript optimizer off for this site (Chrome's site setting, Android's Advanced Protection, Edge's Enhanced
  // security): Chrome then has no WebAssembly at all, or only its interpreter, which can't compile the game (shell.ts)
  if (typeof WebAssembly === 'undefined') return check('isolation', label, 'fail', NO_JIT, 'PAGE-NO-JIT');
  // shared memory at all, of one page and no more: a 32-bit Chrome (most phones with up to 4 GB) sets aside up to
  // 1 GB of its addresses for a memory's maximum until the memory is collected, the gigabyte the game asks for at
  // Play (host/memory.ts); whether the game gets its memory is Play's question
  try { new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true }); }
  catch (e) { return check('isolation', label, 'fail', N('Shared WebAssembly memory is not available: {error}'), 'PAGE-NO-MEMORY', { error: String(e) }); }
  if (typeof (Atomics as { waitAsync?: unknown }).waitAsync !== 'function') {
    return check('isolation', label, 'fail', N('This browser is too old for the game (no Atomics.waitAsync). Update {browser}.'), 'PAGE-NO-WAITASYNC', { browser: browserName() });
  }
  return check('isolation', label, 'ok', N('HTTPS or localhost, cross-origin isolated, threads and shared memory available.'));
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
      ? fail(N('Firefox for Android has no WebGPU yet. Play in Chrome.'), 'GPU-OFF-FIREFOX-ANDROID')
      : fail(N('This Firefox has no WebGPU. On Windows and Apple-chip Macs, update Firefox (Help, About Firefox). Elsewhere, such as Linux, play in Chrome or set dom.webgpu.enabled to true in about:config.'), 'GPU-OFF-FIREFOX')
    : chromium
      ? fail(N('This Chrome has no WebGPU. Update Chrome (Help, About Google Chrome). On Linux, also turn on chrome://flags/#enable-unsafe-webgpu and restart Chrome.'), 'GPU-OFF')
      : ios
        ? fail(N('This browser has no WebGPU. It comes with iOS 26 and iPadOS 26: update this device (Settings, General, Software Update).'), 'GPU-OFF-IOS')
        : fail(N('This browser has no WebGPU. Play in Chrome or Firefox.'), 'GPU-OFF-OTHER');
  let ad: GPUAdapter | null = null;
  try { ad = await gpu.requestAdapter({ powerPreference: 'high-performance' }); } catch { /* reported below */ }
  if (!ad) {
    // the graphics went during the last game, and the reloaded page gets no adapter: the browser keeps them off for a
    // while, not graphics too old (two players' Intel Iris Xe, a hung Direct3D 12 device, then GPU-ADAPTER-WIN in Chrome
    // and Brave).  Chrome blocks a site whose graphics hung from them for two minutes (gpu_data_manager_impl_private.cc,
    // kBlockedDomainExpirationPeriod); the page asks again until they are back (shell.ts recheckGpu)
    if (lostBefore) {
      const b = browserName();
      return fail(N('The graphics stopped during your last game and {browser} hasn\'t given them back yet. Wait a minute or two (the page checks every 10 seconds). If Play stays off, close every {browser} window and open it again. If it keeps happening, update the graphics driver.'), 'GPU-ADAPTER-AFTER-LOST', { browser: b });
    }
    // Firefox has one list of graphics drivers it leaves out, and on Windows, as Chrome, Direct3D 12 only
    if (ff) return fail(N('Firefox won\'t use this computer\'s graphics: they or their driver may be too old. Update the driver or try Chrome (about:support, Graphics, says why).'), 'GPU-ADAPTER-FIREFOX');
    if (chromium) {
      // Chrome 121: Android 12 and newer, on some graphics chips; the unsafe flag lifts its blocklist (Samsung's
      // Xclipse runs the game with it)
      if (android) return fail(N('Chrome can\'t use this device\'s graphics yet: it needs Android 12 or newer, and not every chip is supported. Update Android and Chrome. If that doesn\'t help, turn on chrome://flags/#enable-unsafe-webgpu (type it into the address bar) and restart Chrome: that works on some phones, such as Samsungs with Xclipse graphics.'), 'GPU-ADAPTER-ANDROID');
      // graphics acceleration off in Chrome's settings (or graphics it uses for nothing): WebGL draws in software, or not at all
      const gl = webglRenderer();
      if (gl === null || /SwiftShader|llvmpipe|softpipe|Basic Render/i.test(gl))
        return fail(N('Chrome isn\'t using this computer\'s graphics. Turn on graphics acceleration in chrome://settings/system and restart Chrome. If it\'s already on, update the graphics driver.'), 'GPU-ACCEL-OFF');
      // Windows on ARM (Snapdragon): WebGPU behind a flag in Chrome
      if (windows && await architecture() === 'arm')
        return fail(N('On Windows with an ARM chip (such as Snapdragon), Chrome keeps WebGPU off. Turn on chrome://flags/#enable-unsafe-webgpu and restart Chrome.'), 'GPU-ADAPTER-WIN-ARM');
      // Chrome on Linux runs WebGPU on Vulkan while the rest of it stays on OpenGL, so
      // chrome://gpu says WebGPU is on even when no Vulkan driver is installed
      if (linux) return fail(N('Chrome couldn\'t use this computer\'s graphics, most likely because the Vulkan driver is missing. Install it (mesa-vulkan-drivers on Debian, Ubuntu and Fedora) and restart Chrome. If chrome://gpu says WebGPU is disabled, also turn on chrome://flags/#enable-unsafe-webgpu.'), 'GPU-ADAPTER-LINUX');
      // and on Windows on Direct3D 12 only: the D3D11 adapters are compatibility mode, blocklisted
      if (windows) return fail(N('Chrome couldn\'t use this computer\'s graphics. On Windows it needs DirectX 12, which older graphics (such as Intel HD 4000) lack, so the game can\'t run on them. On newer graphics, update the driver and restart Chrome.'), 'GPU-ADAPTER-WIN');
    }
    // macOS, ChromeOS, another browser: a crashed graphics process (a restart), or an old system
    const b = browserName();
    return fail(N('{Browser} couldn\'t use this computer\'s graphics. Restart {browser}; if that doesn\'t help, update {browser} and the system.'), 'GPU-ADAPTER',
      { browser: b, Browser: b === 'the browser' ? N('The browser') : b });
  }
  let info = ad.info ? [ad.info.vendor, ad.info.architecture, ad.info.description].filter(Boolean).join(' ') : '';
  const ffg = ff && windows && !info ? firefoxGraphics() : null;
  if (ffg) info = ffg.name;
  const named = info ? N('Graphics: {info}') : label;
  // the processor standing in for the graphics (Chrome's SwiftShader, Mesa's llvmpipe): what Chrome hands out with its
  // WebGPU flag where it can't use the graphics, as on a Steam Deck (Chrome on Linux with Vulkan off): the game runs
  // at ~13 fps.  That player's fix was Vulkan for Chrome and for ANGLE (the flags below, as in Chrome 156 on Linux)
  if (chromium && ((ad.info as { isFallbackAdapter?: boolean } | undefined)?.isFallbackAdapter || /swiftshader|llvmpipe|lavapipe|softpipe/i.test(info))) {
    return { ...check('gpu', named, 'warn', linux
      ? N('Chrome gave the game its software graphics, which run on the processor, so the game will be very slow. Turn on chrome://flags/#enable-vulkan and chrome://flags/#default-angle-vulkan, then restart Chrome.')
      : N('Chrome isn\'t using this computer\'s graphics. Turn on graphics acceleration in chrome://settings/system and restart Chrome. If it\'s already on, update the graphics driver.'),
    'GPU-SOFTWARE', { info }), adapter: info };
  }
  // no BC (PowerVR, Mali): the renderer unpacks the textures (web/src/gpu/dxt.ts), so it plays.  With ASTC (iPhones,
  // iPads, most phones) the most of them go up as ASTC instead (astc.ts): ~50 MB more, not 200, nothing to say
  if (!ad.features.has('texture-compression-bc') && !ad.features.has('texture-compression-astc')) {
    return { ...check('gpu', named, 'warn', N('This device can\'t read the game\'s packed textures, so the page unpacks them. The game runs fine but needs about 200 MB more memory, and loading screens may pause briefly.'), 'GPU-NO-BC', { info }), adapter: info };
  }
  if (ffg?.integrated) {
    return { ...check('gpu', named, 'warn', N('Firefox is using the processor\'s built-in graphics. If this computer has an NVIDIA or AMD card, set Firefox to High performance in Windows Settings, System, Display, Graphics, then restart Firefox.'), 'GPU-INTEGRATED-FIREFOX', { info }), adapter: info };
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
  catch { return check('storage', label, 'fail', N('This window can\'t store files (a guest or private window?), so saves won\'t work.'), 'STORAGE-NONE'); }
  // the page writes files (createWritable: Safari has it from 26 on) at Play: the registry's first seed, settings.ini,
  // the cloud's saves.  Safari 18 with its WebGPU feature flag on passed every other check, then Play stopped with
  // "createWritable is not a function" (Discord 2026-11-02)
  if (typeof FileSystemFileHandle === 'undefined' || !('createWritable' in FileSystemFileHandle.prototype)) {
    return check('storage', label, 'fail', N('This browser is too old for the game: it needs iOS 26, iPadOS 26 or Safari 26. Update this device (Settings, General, Software Update).'), 'STORAGE-OLD');
  }
  const verdict = (persisted: boolean): Check => persisted
    ? check('storage', label, 'ok', N('Saves and cached game data are kept for good.'))
    : check('storage', label, 'ok', N('Saves and game data stay until the disk is nearly full, when {browser} may clear them.'), undefined, { browser: browserName() });
  const ask = (async () => await navigator.storage.persisted() || await navigator.storage.persist())()
    .catch(() => false);                 // not decisive
  const quick = await Promise.race([ask, new Promise<null>(res => setTimeout(() => res(null), 500))]);
  if (quick !== null) return verdict(quick);
  const b = browserName();
  return { ...check('storage', label, 'warn', N('{Browser} asks by the address bar to keep this site\'s data for good: allow it so your saves survive a full disk.'),
    'STORAGE-ASK', { Browser: b === 'the browser' ? N('The browser') : b }), answer: ask.then(verdict) };
}

export function fmtBytes(n: number): string {
  if (n >= 1e9) return `${(n / 1e9).toFixed(1)} GB`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(n >= 1e8 ? 0 : 1)} MB`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(0)} KB`;
  return `${n} bytes`;
}
