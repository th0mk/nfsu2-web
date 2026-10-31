// The AudioWorklet processor: copies the mixer's float32 stereo frames out of
// the ring in shared wasm memory (runtime/web/audio_proto.h) into each
// render quantum, then advances the read counter and notifies the pump
// thread waiting on it.  It never blocks (Atomics.wait throws in a worklet)
// and never allocates.  When the ring holds too little it plays what there
// is, then silence, and counts an underrun; the pump raises its target.
// A context that did not get 44.1 kHz resamples linearly here.
// The page ends it for good with the message 'stop' (its game ended): the
// views of the game's memory go at once, and the next process() says 'stopped'
// and returns false.  Only a processor that returned false lets Chrome drop
// its AudioWorkletNode (and the context it belongs to); one that returns true
// forever keeps both alive as long as the page lives.
import {
  AR_HDR_WORDS, AR_LOUD, AR_OFF_PCM, AR_QUANTA, AR_R, AR_UNDERRUNS, AR_W, AUDIO_RATE, AUDIO_RING_FRAMES,
} from './proto';

// AudioWorkletGlobalScope (TypeScript ships no lib for it)
declare const sampleRate: number;
declare class AudioWorkletProcessor { readonly port: MessagePort; constructor(options?: unknown); }
declare function registerProcessor(name: string, ctor: new (options: { processorOptions: Opts }) => AudioWorkletProcessor): void;
interface Opts { buf: SharedArrayBuffer; ptr: number }

const SILENT = 1 / 32768;

class NfsuOut extends AudioWorkletProcessor {
  private h: Int32Array | null;
  private pcm: Float32Array | null;
  private step: number;
  private frames = AUDIO_RING_FRAMES;
  private frac = 0;               // resampling: position between ring frames r and r+1

  constructor(o: { processorOptions: Opts }) {
    super();
    const { buf, ptr } = o.processorOptions;
    this.h = new Int32Array(buf, ptr, AR_HDR_WORDS);
    this.pcm = new Float32Array(buf, ptr + AR_OFF_PCM, this.frames * 2);
    this.step = AUDIO_RATE / sampleRate;
    this.port.onmessage = (e: MessageEvent) => { if (e.data === 'stop') { this.h = null; this.pcm = null; } };
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const h = this.h, pcm = this.pcm;
    if (!h || !pcm) {
      // stopped: the output stays silent, and false ends this processor
      this.port.onmessage = null;
      this.port.postMessage('stopped');
      return false;
    }
    const out = outputs[0];
    if (!out || !out.length) return true;
    const L = out[0], R = out[1] ?? out[0], n = L.length, MASK = this.frames - 1;
    Atomics.add(h, AR_QUANTA, 1);
    const w = Atomics.load(h, AR_W);
    let r = Atomics.load(h, AR_R);
    let avail = (w - r) >>> 0;
    if (avail > this.frames) avail = 0;
    let peak = 0, k = 0;
    if (this.step === 1) {
      k = Math.min(n, avail);
      for (let i = 0; i < k; i++) {
        const j = ((r + i) & MASK) << 1, a = pcm[j], b = pcm[j + 1];
        L[i] = a; R[i] = b;
        const m = a > 0 ? a : -a, m2 = b > 0 ? b : -b;
        if (m > peak) peak = m; if (m2 > peak) peak = m2;
      }
      r = (r + k) | 0;
    } else {
      // linear interpolation between ring frames; needs floor(frac + step*n) + 1 frames
      let f = this.frac, used = 0;
      for (; k < n; k++) {
        const i0 = Math.floor(f);
        if (i0 + 1 >= avail) break;
        const t = f - i0, j0 = ((r + i0) & MASK) << 1, j1 = ((r + i0 + 1) & MASK) << 1;
        const a = pcm[j0] + (pcm[j1] - pcm[j0]) * t, b = pcm[j0 + 1] + (pcm[j1 + 1] - pcm[j0 + 1]) * t;
        L[k] = a; R[k] = b;
        const m = a > 0 ? a : -a, m2 = b > 0 ? b : -b;
        if (m > peak) peak = m; if (m2 > peak) peak = m2;
        f += this.step;
      }
      used = Math.floor(f);
      this.frac = f - used;
      r = (r + used) | 0;
    }
    if (k < n) {
      for (let i = k; i < n; i++) { L[i] = 0; R[i] = 0; }
      Atomics.add(h, AR_UNDERRUNS, 1);
    }
    if (peak > SILENT) Atomics.add(h, AR_LOUD, 1);
    Atomics.store(h, AR_R, r);
    Atomics.notify(h, AR_R, 1);
    return true;
  }
}
registerProcessor('nfsu2-out', NfsuOut);
