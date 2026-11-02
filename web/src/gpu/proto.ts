// The page side of runtime/web/gpu_proto.h: byte offsets of the structures the
// game's recorder (runtime/web/gpu_web.c) writes into the wasm memory.  Keep in
// step with the C header; web/tools/check-gpu-proto.mjs compares the two and
// the renderer refuses a control block whose layout word differs.

export const GW_MAGIC = 0x55504757;
export const GW_VERSION = 5;
export const GW_NBUF = 2;
// GwHL: the other cars' headlights at this byte offset of every frame's VS constant arena (group 0 binding 3)
export const K_HL = 256, HL_BYTES = 656, HL_MAX = 8;
// GwMarks: quick chat's places over the cars at this byte offset of every frame's VS constant arena (not bound): magic,
// n, 2 words, then n of { id, x, y, scale } (x, y: parts of the back buffer from its top left corner)
export const K_MARKS = 1024, MARKS_BYTES = 528, MARKS_MAX = 32, MARKS_MAGIC = 0x4B52414D;
// GwTone: Exposure and White Level (runtime/tonemenu.c) at this byte offset of every frame's VS constant arena (not
// bound): magic, then mult, white and knee (floats); the composite's TONE pass
export const K_TONE = 1552, TONE_BYTES = 16, TONE_MAGIC = 0x454E4F54;
// GwLat: input age (runtime/framerate.c) at this byte offset of every frame's VS constant arena (not bound): magic, a
// word, then (float64) when the game read the input the frame simulates with and when it handed the frame off, in ms
// of performance.timeOrigin + performance.now()
export const K_LAT = 1568, LAT_BYTES = 24, LAT_MAGIC = 0x4E54414C;
// GwPhoto: photo mode's picture (runtime/photo.c) at this byte offset of every frame's VS constant arena (not bound):
// magic, state (0 off, 1 the frame as it is, 2 frame n of target frames of still picture gen, 3 it has them, 4 the frame as it is is the picture), n, target,
// gen; at PHOTO_ACK the page's answer in both buffers' blocks: gen << 16 | the frames of it the page has
export const K_PHOTO = 1600, PHOTO_BYTES = 32, PHOTO_MAGIC = 0x544F4850, PHOTO_ACK = 20;
// the VS constant arena is bound in windows of this many 16-byte rows (GW_KWIN_ROWS)
export const KWIN_ROWS = 4096;

// GwDraw: 156 bytes, all 32-bit fields (word index = byte offset / 4).  A blit's first and count: its source
// rectangle, left | top << 16 and right | bottom << 16 (count 0 = the whole source)
export const DRAW_BYTES = 156, DRAW_WORDS = 39;
export const enum D {
  first = 0, count = 1, tex = 2, target = 3, kind = 4, blend = 5, aref = 6, depth = 7,
  tex1 = 8, tex2 = 9, tex3 = 10, ps = 11, psci = 12, vsid = 13, rawoff = 14, koff = 15,
  proj = 16, smp = 20, blitSrc = 24, bcol = 25, cull = 26, fogv = 27, clrArgb = 31, clrFlags = 32,
  clrZ = 33, ksz = 34, ixIsz = 35, ixIb = 36, ixBase = 37, clip = 38,
}
export const enum Kind { Draw = 0, Clear = 1, Blit = 2 }

// GwFrame: 156 bytes.  rbId nonzero: after the frame, texture rbTex's level 0 goes to the game, rbRows rows of
// rbPitch bytes at wasm address rbDst (see C.rbWant)
export const FRAME_BYTES = 156;
export const enum F {
  seq = 0, dn = 1, vn = 2, rvn = 3, kn = 4, pscn = 5, draws = 6, tv = 7, raw = 8, k = 9, psc = 10,
  rqEnd = 11, bbW = 12, bbH = 13, clearArgb = 14, poolSize = 15, dump = 16, dumpFrame = 17, dumpLabel = 18, // byte 72, 64 chars
  rbId = 34, rbTex = 35, rbDst = 36, rbPitch = 37, rbRows = 38,
}

// GwCtl (word indices)
export const CTL_BYTES = 96 + 2 * FRAME_BYTES;
export const enum C {
  magic = 0, version = 1, layout = 2, size = 3, state = 4, bell = 5, attached = 6, rqHead = 7, rqTail = 8,
  rqPtr = 9, rqCap = 10, framesDone = 11, viewW = 12, viewH = 13, dpr1000 = 14, features = 15,
  stDraws = 16, stPasses = 17, stEncUs = 18, stErrors = 19, dumpsDone = 20, stPipelines = 21, rbWant = 22, rbDone = 23,
  frame = 24,
}
// a texture read back: the page claims the request in rbWant (CAS rbId -> rbId | RB_FLAG; the game cancels one it
// stopped waiting for with a CAS to 0) before it writes rbDst, then stores rbDone = rbId (written) or
// rbId | RB_FLAG (could not read it) and notifies
export const RB_FLAG = 0x80000000;
export const FEAT_BC = 1, FEAT_SWIZZLE = 2;
// GwCtl.attached: 0 before a renderer attached, then ATTACHED, and DETACHED for good once it let go
// (or once the game stopped waiting for one: its CAS 0 -> DETACHED after 30 s)
export const ATTACHED = 1, DETACHED = 2;

// GwVsync: tick i32 @0, seq i32 @4, ts_ms f64 @8, period_ms f64 @16, hz i32 @24, visible i32 @28
export const enum V { tick = 0, seq = 1, tsF64 = 1, periodF64 = 2, hz = 6, visible = 7 }

// resource queue
export const CMD_HDR = 32;
export const enum Op { Pad = 0, TexCreate = 1, TexUpload = 2, TexFree = 3, PoolGrow = 4, PoolWrite = 5, ShaderVs = 6, ShaderPs = 7, ZForget = 8 }
export const TF_RT = 1, TF_CUBE = 2;

// TVert (runtime/gfx.h): 28 floats
export const TVERT_BYTES = 112;

export const LAYOUT = ((DRAW_BYTES | (FRAME_BYTES << 8) | ((CTL_BYTES & 0xFFF) << 16)) ^ (GW_VERSION << 28)) >>> 0;

// D3DFMT values the game creates
export const enum Fmt { A8R8G8B8 = 21, X8R8G8B8 = 22, A8 = 28, P8 = 41, DXT1 = 0x31545844, DXT3 = 0x33545844, DXT5 = 0x35545844 }
