// The movies' pictures in AV1 (tools/movies_av1.py): the game's own movie player reads an AV1 movie as it reads a
// VP6 one, and runtime/movie_av1.c hands each AV1 frame to the I/O worker (Op.MovieDecode), which decodes it here with
// a WebCodecs VideoDecoder and writes the picture into the game's memory, in the planes its VP6 decoder would have
// filled.  The game thread waits for that (one frame: under 1 ms at the median, ~10 at worst, in Chrome and
// Firefox with the software decoder); the I/O worker goes on serving the other threads' reads meanwhile.
//
// Where a browser decodes AV1, the page plays MOVIES/AV1/<name>.vp6 in place of MOVIES/<name>.vp6 (io-worker.ts
// dataFile, datacache.ts av1Path); elsewhere the VP6 movies play as they always have.  NFSU2_MOVIE_CODEC = vp6 (a
// settings.ini line, or &env= on the test pages) keeps the VP6 ones in any browser.
import { MOVIE_SIZE } from '@game/movies';

/** profile 0, level 3.0 (up to 665,856 pixels at 30 fps: Underground 2's 768x512 movies, Most Wanted's 1024x512), 8
 *  bits */
export const AV1_CODEC = 'av01.0.04M.08';
/** the picture of one frame, at most this long (then the frame before stays on screen) */
const WAIT_MS = 250;

export type MovieCodec = 'av1' | 'vp6';

/** Which movies this browser plays: AV1 where its VideoDecoder takes AV1 (Chrome and Firefox with their software
 *  decoder, the same in a worker as here; Safari where the Mac or iPhone decodes AV1 in hardware: it says yes to the
 *  software preference too), else VP6 (Safari on the others).  It asks about the game's movie size (size: the
 *  game's profile, src/games/<id>/movies.ts). */
export async function movieCodec(env: Record<string, string> = {}, size: { w: number; h: number } = MOVIE_SIZE): Promise<MovieCodec> {
  const forced = env.NFSU2_MOVIE_CODEC?.trim().toLowerCase();
  if (forced === 'vp6' || forced === 'av1') return forced;
  try {
    if (typeof VideoDecoder === 'undefined') return 'vp6';
    return (await VideoDecoder.isConfigSupported(config(size.w, size.h))).supported ? 'av1' : 'vp6';
  } catch { return 'vp6'; }
}

function config(w: number, h: number): VideoDecoderConfig {
  // the software decoder: every frame's picture comes out as its chunk goes in (optimizeForLatency; without it
  // Chrome and Firefox hold 2-3 frames back), in I420 in Chrome and BGRX in Firefox; Firefox's hardware decoder
  // left some frames out. Safari's hardware decoder gives each picture at once too, in NV12 (Safari 27, M4 Mac).
  return { codec: AV1_CODEC, codedWidth: w, codedHeight: h, hardwareAcceleration: 'prefer-software', optimizeForLatency: true };
}

interface Movie {
  w: number; h: number;
  dec: VideoDecoder | null;
  seq: number;                      // the timestamp of the last chunk sent
  key: boolean;                     // the decoder needs a key frame next (new, or after an error)
  frame: VideoFrame | null;         // the picture of chunk seq, once out
  wake: (() => void) | null;
  error: string;
  tmp: Uint8Array | null;           // for the pictures that need converting (NV12, RGB)
}
const movies = new Map<number, Movie>();
let nextId = 1;
/** runtime/movie_av1.c HANDLE: 'MV' in the top half, so that a page without these imports (its stub's errno) is no handle */
const TAG = 0x4d560000;

function start(m: Movie) {
  m.dec = new VideoDecoder({
    output: f => {
      if (f.timestamp !== m.seq || m.frame) { f.close(); return; }    // a late one, for a chunk given up on
      m.frame = f; m.wake?.();
    },
    error: e => { m.error = String(e); m.dec = null; m.key = true; m.wake?.(); },
  });
  m.dec.configure(config(m.w, m.h));
  m.key = true;
}

export function movieOpen(w: number, h: number): number {
  const id = TAG | (nextId++ & 0xffff);
  const m: Movie = { w, h, dec: null, seq: 0, key: true, frame: null, wake: null, error: '', tmp: null };
  try { start(m); } catch { return -1; }
  movies.set(id, m);
  return id;
}

export function movieClose(id: number) {
  const m = movies.get(id);
  if (!m) return;
  movies.delete(id);
  m.frame?.close();
  try { m.dec?.close(); } catch { /* closed by an error */ }
}

/** One AV1 frame (the chunk's payload: 'AV01', flags, 3 bytes, the temporal unit) into the planes y, u, v of the
 *  game's memory (strides ys, uvs).  0: the picture is there; 1: none came (the last one stays). */
export async function movieDecode(memory: WebAssembly.Memory, id: number, payload: number, len: number,
    y: number, u: number, v: number, ys: number, uvs: number, log: (s: string) => void): Promise<number> {
  const m = movies.get(id);
  if (!m || len < 8) return 1;
  const head = new Uint8Array(memory.buffer, payload, 8), key = (head[4] & 1) !== 0;
  if (!m.dec) {
    if (!key) return 1;                                            // after an error: from the next key frame on
    try { start(m); } catch { return 1; }
  }
  if (m.key && !key) return 1;
  m.frame?.close(); m.frame = null; m.error = '';
  m.seq++;
  // the chunk takes its own copy (the guest's memory is shared)
  const data = new Uint8Array(memory.buffer, payload + 8, len - 8).slice();
  try { m.dec!.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: m.seq, data })); }
  catch (e) {
    log(`[movie] decode refused: ${e}`);
    try { m.dec?.close(); } catch { /* closed already */ }
    m.dec = null; return 1;
  }
  m.key = false;
  if (!m.frame) {
    let timer: ReturnType<typeof setTimeout> | undefined;
    await new Promise<void>(r => { m.wake = r; timer = setTimeout(r, WAIT_MS); });
    clearTimeout(timer); m.wake = null;
  }
  if (m.error) log(`[movie] decoder error: ${m.error}`);
  const f = m.frame as VideoFrame | null;     // set by the output callback meanwhile
  if (!f) return 1;
  m.frame = null;
  try { await put(memory, m, f, y, u, v, ys, uvs); return 0; }
  catch (e) { log(`[movie] picture not copied (${f.format}): ${e}`); return 1; }
  finally { f.close(); }
}

async function put(memory: WebAssembly.Memory, m: Movie, f: VideoFrame, y: number, u: number, v: number, ys: number, uvs: number) {
  const w = Math.min(m.w, f.visibleRect?.width ?? m.w) & ~1, h = Math.min(m.h, f.visibleRect?.height ?? m.h) & ~1;
  const rect = { x: f.visibleRect?.x ?? 0, y: f.visibleRect?.y ?? 0, width: w, height: h };
  const span = v - y + uvs * (h / 2);
  if (f.format === 'I420') {
    const layout = [{ offset: 0, stride: ys }, { offset: u - y, stride: uvs }, { offset: v - y, stride: uvs }];
    try { await f.copyTo(new Uint8Array(memory.buffer, y, span), { rect, layout }); return; }
    catch { /* a browser that copies into no shared memory: through a copy of its own */ }
    const t = tmp(m, span);
    await f.copyTo(t, { rect, layout });
    new Uint8Array(memory.buffer, y, span).set(t.subarray(0, span));
    return;
  }
  const mem = new Uint8Array(memory.buffer);
  if (f.format === 'NV12') {
    const t = tmp(m, w * h * 3 / 2);
    await f.copyTo(t, { rect, layout: [{ offset: 0, stride: w }, { offset: w * h, stride: w }] });
    for (let r = 0; r < h; r++) mem.set(t.subarray(r * w, r * w + w), y + r * ys);
    for (let r = 0, o = w * h; r < h / 2; r++) {
      const du = u + r * uvs, dvv = v + r * uvs;
      for (let c = 0; c < w / 2; c++, o += 2) { mem[du + c] = t[o]; mem[dvv + c] = t[o + 1]; }
    }
    return;
  }
  // RGB (Firefox gives its pictures as BGRX), or anything else as RGBX: back to the YUV the frame was made from,
  // by its own matrix (limited range)
  const rgb = f.format === 'RGBX' || f.format === 'RGBA' || f.format === 'BGRX' || f.format === 'BGRA';
  const t = tmp(m, w * h * 4);
  await f.copyTo(t, rgb ? { rect, layout: [{ offset: 0, stride: w * 4 }] } : { rect, layout: [{ offset: 0, stride: w * 4 }], format: 'RGBX' } as VideoFrameCopyToOptions);
  const bgr = f.format === 'BGRX' || f.format === 'BGRA';
  const mat = f.colorSpace?.matrix;
  const kr = mat === 'bt470bg' || mat === 'smpte170m' ? 0.299 : 0.2126, kb = mat === 'bt470bg' || mat === 'smpte170m' ? 0.114 : 0.0722;
  toI420(t, w, h, bgr, kr, kb, mem, y, u, v, ys, uvs);
}

function tmp(m: Movie, n: number) {
  if (!m.tmp || m.tmp.length < n) m.tmp = new Uint8Array(n);
  return m.tmp;
}

/** RGB(X) pixels (BGRX if bgr) -> limited-range Y'CbCr 4:2:0 by the matrix of Kr and Kb, chroma from each 2x2's mean */
export function toI420(t: Uint8Array, w: number, h: number, bgr: boolean, kr: number, kb: number,
    mem: Uint8Array, y: number, u: number, v: number, ys: number, uvs: number) {
  const kg = 1 - kr - kb, sy = 219 / 255, sb = 224 / 255 / (2 * (1 - kb)) / 4, sr = 224 / 255 / (2 * (1 - kr)) / 4;
  const yr = sy * kr, yg = sy * kg, yb = sy * kb, ri = bgr ? 2 : 0, bi = bgr ? 0 : 2, row = w * 4;
  for (let r = 0; r < h; r += 2) {
    let o = r * row, d = y + r * ys, cu = u + (r >> 1) * uvs, cv = v + (r >> 1) * uvs;
    for (let c = 0; c < w; c += 2, o += 8, d += 2) {
      // a 2x2 block: its four lumas, and the chroma of its sums (Uint8Array stores clamp nothing: Y stays in
      // 16..235 by the matrix, the chroma is clamped below)
      const r0 = t[o + ri], g0 = t[o + 1], b0 = t[o + bi], r1 = t[o + 4 + ri], g1 = t[o + 5], b1 = t[o + 4 + bi];
      const r2 = t[o + row + ri], g2 = t[o + row + 1], b2 = t[o + row + bi];
      const r3 = t[o + row + 4 + ri], g3 = t[o + row + 5], b3 = t[o + row + 4 + bi];
      mem[d] = 16.5 + yr * r0 + yg * g0 + yb * b0; mem[d + 1] = 16.5 + yr * r1 + yg * g1 + yb * b1;
      mem[d + ys] = 16.5 + yr * r2 + yg * g2 + yb * b2; mem[d + ys + 1] = 16.5 + yr * r3 + yg * g3 + yb * b3;
      const R = r0 + r1 + r2 + r3, G = g0 + g1 + g2 + g3, B = b0 + b1 + b2 + b3, Y = kr * R + kg * G + kb * B;
      const cb = 128.5 + sb * (B - Y), cr = 128.5 + sr * (R - Y);
      mem[cu++] = cb < 0 ? 0 : cb > 255 ? 255 : cb;
      mem[cv++] = cr < 0 ? 0 : cr > 255 ? 255 : cr;
    }
  }
}
