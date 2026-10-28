// The game's sound output: an AudioContext with an AudioWorklet that plays the
// mixer's ring (runtime/web/audio_web.c, audio_proto.h).
//
// The C side runs the mixer on its own "audio pump" thread and keeps the
// DirectSound play cursors moving on the wall clock whenever this page is not
// consuming (no context yet, suspended before the first gesture, interrupted,
// stalled, sound off), so nothing here is needed for the game to run: this
// module only makes it audible.
//   - The context is the one the page made inside its Play click (opts.context),
//     or one created here at once.  Chrome starts a context suspended until the
//     page has had a click or a key press; the first pointer or key press
//     resumes it (and any later one, should the context be interrupted).  A
//     gamepad button is not such a gesture (Chrome gives pads no user
//     activation): it resumes the context only once the page already had a
//     click or key press, e.g. the Play click.
//   - Latency: the ring holds 512-1024 frames (12-23 ms; the pump raises it
//     after underruns and lowers it again after 20 s without one) on top of
//     the context's own output latency; with a context that renders in big
//     bursts (some phones: ~30 quanta every 90 ms) one burst and a slice.
//   - Hidden page: the output fades to silence (the game itself parks and
//     mutes when it loses focus); optionally the context is suspended.
//   - autotest "audioreset": the pump bumps AR_RESET and the context is rebuilt.
import type { GameProcess } from '../host/process';
import { HostKind } from '../host/abi';
import { abs } from '../base';
import {
  AR_CTX, AR_CTX_CLOSED, AR_CTX_RUNNING, AR_CTX_SUSPENDED, AR_FRAMES, AR_HDR_WORDS, AR_LOUD, AR_MAGIC, AR_MIXED, AR_MODE, AR_MUTED,
  AR_NOSOUND, AR_OFF_PCM, AR_PAGE_FRAMES, AR_PAGE_RATE, AR_PEAK, AR_QUANTA, AR_R, AR_RESET, AR_RMS, AR_TARGET, AR_UNDERRUNS,
  AR_VERSION, AR_W, AR_WALL_MS, AUDIO_MAGIC, AUDIO_RATE, AUDIO_RING_FRAMES, AUDIO_RING_FRAMES_OLD, AUDIO_VERSION,
} from './proto';

export interface AudioOptions {
  /** a context to play into, e.g. made inside the page's Play click (a user gesture);
   *  it is used as is (its sample rate and latency) and closed with this handle.
   *  Without it one is made here: 44.1 kHz, opts.latencyHint. */
  context?: AudioContext | null;
  volume?: number;                                   // 0..1 master volume (default 1)
  muteWhenHidden?: boolean;                          // default true
  suspendWhenHidden?: boolean;                       // also suspend the context (default false)
  latencyHint?: AudioContextLatencyCategory | number; // default 'interactive'
  gestureTarget?: EventTarget;                       // where to listen for the unlocking gesture (default window)
  log?: (s: string) => void;
}
export interface AudioStats {
  attached: boolean; nosound: boolean; muted: boolean; state: string; mode: 'idle' | 'ring' | 'wall';
  fill: number; target: number; underruns: number; quanta: number; loudQuanta: number;
  peakDb: number; rmsDb: number; wallMs: number; mixedFrames: number;
  baseLatencyMs: number; outputLatencyMs: number; sampleRate: number; outputGain: number;
}
/** the numeric counters (the app's overlay contract: names to numbers) */
export interface AudioCounters {
  [k: string]: number;
  'audio fill ms': number; 'audio underruns': number; 'audio latency ms': number; 'audio running': number;
}
export interface AudioHandle {
  readonly context: AudioContext | null;
  resume(): Promise<void>;
  setVolume(v: number): void;
  /** everything, for test pages and logs */
  status(): AudioStats;
  /** numbers only, for the app's detailed overlay */
  stats(): AudioCounters;
  close(): Promise<void>;
  /** close() without waiting: listeners, the poll and the context go at once */
  detach(): void;
}

const db = (q16: number) => (q16 > 0 ? 20 * Math.log10(q16 / 65536) : -140);

/** Ends a context and its worklet node for good, so neither keeps the game's
 *  memory reachable after its game ended.  Outside attachAudio on purpose: a
 *  closure made in there would hold its scope, and with it the game process.
 *  - The context's handler goes first: a closed context can outlive the close
 *    (its node keeps it), and the handler would keep the game.
 *  - The processor gets 'stop': it drops its views of the game's memory and its
 *    process() returns false.  Chrome keeps an AudioWorkletNode (and its context)
 *    alive as long as the processor has not returned false, and a closed context
 *    never calls process() again, so the close waits for the processor's
 *    'stopped' (at most 250 ms; a context that is not running closes at once). */
function endOutput(c: AudioContext, n: AudioWorkletNode | null, log: (s: string) => void) {
  c.onstatechange = null;
  const close = () => void c.close().then(() => log('[audio] context closed'), () => {});
  if (!n) { close(); return; }
  n.port.postMessage('stop');
  if (c.state !== 'running') { try { n.disconnect(); } catch { /* already gone */ } close(); return; }
  let done = false;
  const end = () => {
    if (done) return;
    done = true;
    n.port.onmessage = null; n.port.close();
    try { n.disconnect(); } catch { /* already gone */ }
    close();
  };
  n.port.onmessage = (e) => { if (e.data === 'stopped') end(); };
  setTimeout(end, 250);
}

export async function attachAudio(proc: GameProcess, opts: AudioOptions = {}): Promise<AudioHandle> {
  const log = opts.log ?? ((s: string) => console.log(s));
  const target = opts.gestureTarget ?? window;
  let volume = Math.max(0, Math.min(1, opts.volume ?? 1));
  let ctx: AudioContext | null = null, node: AudioWorkletNode | null = null, gain: GainNode | null = null;
  let h: Int32Array | null = null, ring: { ptr: number; size: number } | null = null;
  let closed = false, hiddenSuspended = false, building: Promise<void> | null = null;

  const ctxWord = () => {
    if (!h || !ctx) return;
    const st = ctx.state as string;
    Atomics.store(h, AR_CTX, st === 'running' && node ? AR_CTX_RUNNING : st === 'suspended' ? AR_CTX_SUSPENDED
      : st === 'running' ? AR_CTX_SUSPENDED : AR_CTX_CLOSED);
  };
  const audible = () => (opts.muteWhenHidden ?? true) && document.hidden ? 0 : volume;

  let given = opts.context && opts.context.state !== 'closed' ? opts.context : null;
  async function build() {
    // the page's context the first time; a rebuild (audioreset) makes its own
    const c = given ?? new AudioContext({ sampleRate: AUDIO_RATE, latencyHint: opts.latencyHint ?? 'interactive' });
    if (given) log(`[audio] playing into the page's AudioContext (${c.sampleRate} Hz, ${c.state})`);
    given = null;
    ctx = c;
    // endOutput() removes it again: nothing of this game may stay on a context that outlives it
    // with how long the state before lasted (an iPhone interrupts the sound for a call, Siri, the lock screen)
    let was = c.state as string, since = performance.now();
    c.onstatechange = () => {
      ctxWord();
      const now = performance.now();
      log(`[audio] context ${c.state}${was !== c.state ? ` (${was} for ${((now - since) / 1000).toFixed(1)} s)` : ''}`);
      was = c.state; since = now;
    };
    try { await c.audioWorklet.addModule(abs('audio/worklet.js')); }
    catch (e) {
      // a context closed meanwhile (sound off, a rebuild) aborts the load
      if (!closed && ctx === c) log(`[audio] the worklet did not load: ${e}; no sound`);
      return;
    }
    if (closed || ctx !== c) return;
    gain = c.createGain(); gain.gain.value = audible(); gain.connect(c.destination);
    const connect = () => {
      if (!ring || node || ctx !== c) return;
      // the module's ring size (an older module's is AUDIO_RING_FRAMES_OLD), told
      // to the pump before the context reads as running
      const f = h ? Atomics.load(h, AR_FRAMES) : 0;
      const frames = f >= AUDIO_RING_FRAMES_OLD && f <= AUDIO_RING_FRAMES && !(f & (f - 1)) && AR_OFF_PCM + f * 8 <= ring.size
        ? f : AUDIO_RING_FRAMES_OLD;
      if (h) Atomics.store(h, AR_PAGE_FRAMES, frames);
      node = new AudioWorkletNode(c, 'nfsu2-out', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { buf: proc.memory.buffer as unknown as SharedArrayBuffer, ptr: ring.ptr, frames },
      });
      node.connect(gain!);
      if (h) Atomics.store(h, AR_PAGE_RATE, c.sampleRate | 0);
      ctxWord();
      log(`[audio] worklet attached: ${c.sampleRate} Hz, base latency ${(c.baseLatency * 1000).toFixed(1)} ms, ` +
          `output latency ${((c.outputLatency || 0) * 1000).toFixed(1)} ms, state ${c.state}`);
    };
    if (ring) connect(); else proc.registered(HostKind.Audio).then(() => connect());
    ctxWord();
  }
  function teardown() {
    if (h) Atomics.store(h, AR_CTX, AR_CTX_CLOSED);
    const c = ctx, n = node; ctx = null; node = null; gain = null;
    if (c) endOutput(c, n, log);
  }
  async function rebuild(why: string) {
    if (building) return building;
    log(`[audio] rebuilding the AudioContext (${why})`);
    teardown();
    building = build().finally(() => { building = null; });
    return building;
  }

  const resume = async () => {
    if (!ctx || closed) return;
    if (ctx.state !== 'running') { try { await ctx.resume(); } catch (e) { log(`[audio] resume failed: ${e}`); } }
    ctxWord();
  };
  // a click or a key press unlocks a suspended context; the pad event
  // (nfsu2-gesture, from the input layer) only helps once the page already had one
  const onGesture = () => { if (ctx && ctx.state !== 'running' && !(document.hidden && hiddenSuspended)) void resume(); };
  for (const t of ['pointerdown', 'keydown', 'touchend', 'nfsu2-gesture']) target.addEventListener(t, onGesture, { capture: true });

  const onVisibility = () => {
    if (gain && ctx) gain.gain.setTargetAtTime(audible(), ctx.currentTime, 0.015);
    if (opts.suspendWhenHidden && ctx) {
      if (document.hidden && ctx.state === 'running') { hiddenSuspended = true; void ctx.suspend(); }
      else if (!document.hidden && hiddenSuspended) { hiddenSuspended = false; void resume(); }
    }
  };
  document.addEventListener('visibilitychange', onVisibility);

  // the ring arrives when the game creates its first sound buffer
  let resetSeen = 0;
  const poll = setInterval(() => {
    if (!h || closed) return;
    const r = Atomics.load(h, AR_RESET);
    if (r !== resetSeen) { resetSeen = r; void rebuild('requested by the game (audioreset)'); }
  }, 200);
  proc.registered(HostKind.Audio).then(reg => {
    if (closed) return;
    const view = new Int32Array(proc.memory.buffer, reg.ptr, AR_HDR_WORDS);
    if (view[AR_MAGIC] !== AUDIO_MAGIC || view[AR_VERSION] !== AUDIO_VERSION) {
      log(`[audio] ring at 0x${reg.ptr.toString(16)} has magic 0x${(view[AR_MAGIC] >>> 0).toString(16)} version ${view[AR_VERSION]}: not ours, no sound`);
      return;
    }
    h = view; ring = reg; resetSeen = Atomics.load(h, AR_RESET);
    if (Atomics.load(h, AR_NOSOUND)) { log('[audio] sound is off (NOSOUND): no output'); teardown(); return; }
    ctxWord();
  });

  building = build().finally(() => { building = null; });
  await building;

  const close = async () => {
    if (closed) return;
    closed = true; clearInterval(poll);
    for (const t of ['pointerdown', 'keydown', 'touchend', 'nfsu2-gesture']) target.removeEventListener(t, onGesture, { capture: true });
    document.removeEventListener('visibilitychange', onVisibility);
    teardown();
    // no view of the old process's memory stays reachable: the processor drops
    // its own and ends (endOutput), the context loses its handler
    h = null; ring = null;
  };
  const handle: AudioHandle = {
    get context() { return ctx; },
    resume,
    setVolume(v: number) { volume = Math.max(0, Math.min(1, v)); if (gain && ctx) gain.gain.setTargetAtTime(audible(), ctx.currentTime, 0.015); },
    stats(): AudioCounters {
      const st = handle.status();
      const rate = st.sampleRate || AUDIO_RATE;
      return { 'audio fill ms': st.fill * 1000 / AUDIO_RATE, 'audio underruns': st.underruns,
        'audio latency ms': st.baseLatencyMs + st.outputLatencyMs, 'audio running': st.state === 'running' && st.mode === 'ring' ? 1 : 0,
        'audio rate': rate };
    },
    status(): AudioStats {
      const z = (i: number) => (h ? Atomics.load(h, i) >>> 0 : 0);
      const mode = z(AR_MODE);
      return {
        attached: !!node, nosound: !!z(AR_NOSOUND), muted: !!z(AR_MUTED), state: ctx?.state ?? 'none', mode: mode === 1 ? 'ring' : mode === 2 ? 'wall' : 'idle',
        fill: (z(AR_W) - z(AR_R)) >>> 0, target: z(AR_TARGET), underruns: z(AR_UNDERRUNS), quanta: z(AR_QUANTA),
        loudQuanta: z(AR_LOUD), peakDb: db(z(AR_PEAK)), rmsDb: db(z(AR_RMS)), wallMs: z(AR_WALL_MS), mixedFrames: z(AR_MIXED),
        baseLatencyMs: (ctx?.baseLatency ?? 0) * 1000, outputLatencyMs: (ctx?.outputLatency ?? 0) * 1000, sampleRate: ctx?.sampleRate ?? 0,
        outputGain: gain?.gain.value ?? 0,
      };
    },
    close,
    detach() { void close(); },
  };
  return handle;
}
