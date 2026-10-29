// The page side of runtime/web/gpu_proto.h: byte offsets of the structures the
// game's recorder (runtime/web/gpu_web.c) writes into the wasm memory.  Keep in
// step with the C header; web/tools/check-gpu-proto.mjs compares the two and
// the renderer refuses a control block whose layout word differs.

export const GW_MAGIC = 0x55504757;
export const GW_VERSION = 6;
export const GW_NBUF = 2;
// GwHL: the other cars' headlights at this byte offset of every frame's VS constant arena (group 0 binding 3)
export const K_HL = 256, HL_BYTES = 656, HL_MAX = 8;
// GwMarks: quick chat's places over the cars at this byte offset of every frame's VS constant arena (not bound): magic,
// n, 2 words, then n of { id, x, y, scale } (x, y: parts of the back buffer from its top left corner)
export const K_MARKS = 1024, MARKS_BYTES = 528, MARKS_MAX = 32, MARKS_MAGIC = 0x4B52414D;
// GwTone: Exposure and White Level (runtime/tonemenu.c) at this byte offset of every frame's VS constant arena (not
// bound): magic, then mult, white and knee (floats); the composite's TONE pass
export const K_TONE = 1552, TONE_BYTES = 16, TONE_MAGIC = 0x454E4F54;
// the VS constant arena is bound in windows of this many 16-byte rows (GW_KWIN_ROWS)
export const KWIN_ROWS = 4096;

// GwDraw: 188 bytes, all 32-bit fields (word index = byte offset / 4).  A blit's first and count: its source
// rectangle, left | top << 16 and right | bottom << 16 (count 0 = the whole source).  tex4..tex7 and smpHi (the
// sampler keys of stages 4-7) are read only when C.stages is 8
export const DRAW_BYTES = 188, DRAW_WORDS = 47;
export const enum D {
  first = 0, count = 1, tex = 2, target = 3, kind = 4, blend = 5, aref = 6, depth = 7,
  tex1 = 8, tex2 = 9, tex3 = 10, ps = 11, psci = 12, vsid = 13, rawoff = 14, koff = 15,
  proj = 16, smp = 20, blitSrc = 24, bcol = 25, cull = 26, fogv = 27, clrArgb = 31, clrFlags = 32,
  clrZ = 33, ksz = 34, ixIsz = 35, ixIb = 36, ixBase = 37, clip = 38,
  tex4 = 39, tex5 = 40, tex6 = 41, tex7 = 42, smpHi = 43,
}
// D.depth bits 8+ from Z_TEX on: a depth texture's id + Z_TEX, that texture as the draw's Z (below it: the Z surfaces)
export const Z_TEX = 64;
// D.depth bits from ZBIAS_SHIFT up (8 stages): the depth bias, 4 bits constant (x 2^-24) then 4 bits slope scale;
// the Z surface id is bits 8-23
export const ZBIAS_SHIFT = 24;
// D.ps bits from PS_DEPTH_SHIFT up: stage k samples a depth texture (TF_DEPTH) with a depth compare, in the module
// registered under the whole id; ps & 0x1FF is the game's shader (+256: the stage-0 cube twin)
export const PS_DEPTH_SHIFT = 9;
// C.stages: the texture stages of every draw and module, 4 (0 reads as 4) or STAGES_MAX
export const STAGES_MAX = 8;
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
export const CTL_BYTES = 100 + 2 * FRAME_BYTES;
export const enum C {
  magic = 0, version = 1, layout = 2, size = 3, state = 4, bell = 5, attached = 6, rqHead = 7, rqTail = 8,
  rqPtr = 9, rqCap = 10, framesDone = 11, viewW = 12, viewH = 13, dpr1000 = 14, features = 15,
  stDraws = 16, stPasses = 17, stEncUs = 18, stErrors = 19, dumpsDone = 20, stPipelines = 21, rbWant = 22, rbDone = 23,
  stages = 24, frame = 25,
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
// TF_DEPTH: a depth texture (D3DFMT_D16, D24X8, D24S8): depth32float, a draw's Z through Z_TEX, sampled with a
// depth compare; never uploaded or read back
export const TF_RT = 1, TF_CUBE = 2, TF_DEPTH = 4;
// ShaderPs records: a[2] is the module's binding kinds, 2 bits per stage k at bit 2k (gpu_proto.h GW_BK_*)
export const BK_2D = 0, BK_CUBE = 1, BK_DEPTH = 2;

// TVert (runtime/gfx.h): 28 floats
export const TVERT_BYTES = 112;

export const LAYOUT = ((DRAW_BYTES | (FRAME_BYTES << 8) | ((CTL_BYTES & 0xFFF) << 16)) ^ (GW_VERSION << 28)) >>> 0;

// D3DFMT values the game creates
export const enum Fmt {
  A8R8G8B8 = 21, X8R8G8B8 = 22, A8 = 28, P8 = 41, DXT1 = 0x31545844, DXT3 = 0x33545844, DXT5 = 0x35545844,
  D24S8 = 75, D24X8 = 77, D16 = 80,   // depth textures (TF_DEPTH)
}
