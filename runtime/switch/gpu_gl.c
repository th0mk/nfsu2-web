/* gpu_gl.c -- see gpu_gl.h.  The OpenGL 4.3 core renderer: a port of web/src/gpu/encoder.ts onto the game's recorder.
 *
 * Conventions kept from the WebGPU encoder, on purpose: NDC and framebuffer orientation (glClipControl UPPER_LEFT,
 * ZERO_TO_ONE: row 0 of a texture or target is its top, depth 0..1), a front face that is clockwise, the first pass of a
 * target in a frame starts with a clear to the frame's clear colour, Z clears are deferred to the next pass that binds the
 * Z surface, a Z surface is created at the size of the first target it is used with (a mismatch draws without Z), and the
 * state tables of encoder.ts (blend factors, compare functions, sampler keys).  Shaders are the game translator's WGSL,
 * turned into GLSL by wgsl2glsl.c; a (vertex, pixel) pair is linked when a draw first needs it. */
#include "gpu_gl.h"
#include "wgsl2glsl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
/* the console (glad), or the Linux test host, which also builds with -D__SWITCH__ (NFSU2_LINUX_HOST: build-linux.sh) */
#if defined(__SWITCH__) && !defined(NFSU2_LINUX_HOST)
#define GLR_CONSOLE 1
#include <glad/glad.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#define MAXTEX 16384
#define MAXVS  8192
#define MAXPS  1024
#define PSCUBE 256
#define PSLOT  768              /* a PSPar (560 bytes) at 256-aligned slots */
#define KWIN   65536
#define TVERT  112

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_UPPER_LEFT
#define GL_UPPER_LEFT 0x8CA2
#define GL_LOWER_LEFT 0x8CA1
#endif
#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE 0x935F
#endif
/* glClipControl is GL 4.5 / ARB_clip_control: glad's 4.3 header lacks it, the driver has it */
static void (*p_clip_control)(GLenum, GLenum);
static void clip_control(GLenum origin, GLenum depth){ if (p_clip_control) p_clip_control(origin, depth); }
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

typedef struct { GLuint gl; uint32_t fmt, w, h, levels; uint8_t cube, rt, live; } Tex;
/* a cheap clock for the profile of the draw loop: libnx's system tick (19.2 MHz on the console; the raw counter register
 * made b20 close at once) */
#if defined(__aarch64__) && !defined(NFSU2_LINUX_HOST)
#include <switch/arm/counter.h>
static inline uint64_t tick(void){ return armGetSystemTick(); }
#define TICK_HZ 19200000.0
#else
static inline uint64_t tick(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
#define TICK_HZ 1e9
#endif
typedef struct { GLuint prog; int kb, af; int bad; uint32_t key; int last_af, last_kb; } Prog;     /* last_*: the uniform value + 1 (0: unknown) */

static struct {
    EGLDisplay dpy; EGLSurface srf; EGLContext ctx;
    int win_w, win_h, onscreen;
    Tex tex[MAXTEX];
    GLuint white, white_cube;
    char *vs_glsl[MAXVS], *ps_glsl[MAXPS];
    GLuint vs_obj[MAXVS], ps_obj[MAXPS];
    Prog *progs; unsigned nprog, capprog;
    GLuint vao, fbo, fbo_read, fbo_draw;
    GLuint tv_buf[3], raw_buf[3], k_buf[3], p_buf[3], pool_buf;      /* a ring of three sets: a frame never writes what the GPU may still read */
    int tbuf; GLuint tb_tex;                                         /* NFSU2_NX_TBUF=1: the vertex buffer is read as a buffer texture (yuzu cannot build SSBO reads) */
    size_t tv_cap[3], raw_cap[3], k_cap[3], p_cap[3], pool_cap;
    GLuint bb, bb_w, bb_h;
    GLuint z_rbo[64]; unsigned z_w[64], z_h[64]; uint8_t zpend[64]; float zpend_v[64];
    GLuint present_prog; GLint present_flip;
    GLuint ov_prog, ov_tex; GLint ov_rect; volatile int ov_on; int ov_w, ov_h;      /* the fps counter over the picture */
    uint64_t ov_t0; unsigned ov_frames; double ov_ms_rec;
    GLuint samplers[4096]; uint32_t sampler_key[4096]; unsigned nsampler;
    float *pstage; size_t pstage_cap;
    unsigned char *touched; unsigned *touched_list; unsigned ntouched;
    /* stats */
    unsigned long frames_at_report;
    uint32_t *pslot_of; size_t pslot_cap;
    uint64_t pf[8];                  /* ticks per category since the last report: prog, state, bufbind, tex, draw, pre-pass, passes+blits, rest */
    double ms_build, ms_swap;      /* per frame: the time to record the draws into the driver, and the time inside eglSwapBuffers */
    volatile unsigned phase, ph_a, ph_b, ph_c; volatile unsigned long frame_no;
    unsigned long frames, draws, skipped, passes, progs_made, shader_fail, tex_made, tex_up;
    unsigned shot_every, shot_seq; uint32_t *shot[2]; unsigned shot_w, shot_h; unsigned char *rb;
} G;

static void glr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void glr_log(const char *fmt, ...){ va_list ap; va_start(ap, fmt); fprintf(stderr, "[glr] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap); }

/* ---- shaders and programs ---- */
static GLuint compile_stage(GLenum type, const char *src, const char *what){
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok){ char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); glr_log("%s: %s", what, log); glDeleteShader(s); G.shader_fail++; return 0; }
    return s;
}
static void ov_init(void);
static int g_pc_on;      /* the program cache is in use (set by pc_init) */
static GLuint make_prog(const char *vs_src, const char *fs_src, const char *what){
    GLuint v = compile_stage(GL_VERTEX_SHADER, vs_src, what), f = compile_stage(GL_FRAGMENT_SHADER, fs_src, what);
    if (!v || !f){ if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f);
    if (g_pc_on) glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(v); glDeleteShader(f);
    if (!ok){ char log[1024]; glGetProgramInfoLog(p, sizeof log, NULL, log); glr_log("link %s: %s", what, log); glDeleteProgram(p); return 0; }
    return p;
}
void glr_shader(int is_ps, uint32_t id, const char *wgsl, uint32_t len){
    static char out[1 << 17], err[256], in[1 << 17];
    if (id >= (is_ps ? MAXPS : MAXVS) || len >= sizeof in) return;
    memcpy(in, wgsl, len); in[len] = 0;
    int frag = 0;
    int rc = wgsl_to_glsl(in, out, sizeof out, &frag, err, sizeof err);
    if (rc){ glr_log("%s_%u: translate failed (%d): %s", is_ps ? "ps" : "vs", id, rc, err); G.shader_fail++; return; }
    char **slot = is_ps ? &G.ps_glsl[id] : &G.vs_glsl[id];
    free(*slot); *slot = strdup(out);
}

/* ---- the program cache ----
 * A program the game uses for the first time is translated (WGSL -> GLSL, at load) and compiled by the driver in the middle of
 * the race: 40-185 ms each on the console, a visible stop.  The driver's own binary of every program that linked is kept in
 * /switch/nfsu2/progcache.bin (header: the GL version and renderer; records: a hash of the two GLSL sources, the binary format,
 * the bytes), and the next run loads them instead.  A record that does not load (a driver update, a different translator) is just
 * compiled.  Off when the driver has no binary formats, or with an empty file /switch/nfsu2/nocache. */
#ifdef NFSU2_LINUX_HOST
#define PC_PATH_DEFAULT "/tmp/nfsu2-progcache.bin"
#else
#define PC_PATH_DEFAULT "/switch/nfsu2/progcache.bin"
#endif
typedef struct { uint64_t hash; uint32_t fmt, len; size_t off; } PcEnt;
static struct { int on, fresh; PcEnt *e; unsigned n, cap; unsigned char *data; size_t dsz; uint64_t env; unsigned loaded, saved, failed; double ms_loaded; } PC;
static uint64_t fnv64(uint64_t h, const char *p){ for (; *p; p++){ h ^= (unsigned char)*p; h *= 1099511628211ull; } return h; }
static const char *pc_path(void){ const char *e = getenv("NFSU2_PROGCACHE"); return e && *e ? e : PC_PATH_DEFAULT; }
static void pc_init(void){
    GLint nf = 0; glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nf);
#ifndef NFSU2_LINUX_HOST
    struct stat st; if (stat("/switch/nfsu2/nocache", &st) == 0) nf = 0;
#endif
    if (nf <= 0){ glr_log("program cache: off (%d binary formats)", (int)nf); return; }
    PC.on = 1; g_pc_on = 1;
    PC.env = fnv64(fnv64(14695981039346656037ull, (const char *)glGetString(GL_VERSION)), (const char *)glGetString(GL_RENDERER));
    FILE *f = fopen(pc_path(), "rb");
    if (!f){ PC.fresh = 1; glr_log("program cache: new file"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = sz > 12 ? malloc((size_t)sz) : NULL;
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz){ fclose(f); free(d); PC.fresh = 1; return; }
    fclose(f);
    uint64_t env; memcpy(&env, d + 4, 8);
    if (memcmp(d, "NPC1", 4) || env != PC.env){ free(d); PC.fresh = 1; glr_log("program cache: for another driver or format, started again"); return; }
    size_t pos = 12;
    while (pos + 16 <= (size_t)sz){
        PcEnt e; uint32_t len, fmt; uint64_t h;
        memcpy(&h, d + pos, 8); memcpy(&fmt, d + pos + 8, 4); memcpy(&len, d + pos + 12, 4);
        if (pos + 16 + len > (size_t)sz) break;
        e.hash = h; e.fmt = fmt; e.len = len; e.off = pos + 16;
        if (PC.n == PC.cap){ PC.cap = PC.cap ? PC.cap * 2 : 64; PC.e = realloc(PC.e, PC.cap * sizeof *PC.e); }
        PC.e[PC.n++] = e; pos += 16 + len;
    }
    PC.data = d; PC.dsz = (size_t)sz;
    glr_log("program cache: %u programs on file (%.1f KB)", PC.n, sz / 1024.0);
}
static GLuint pc_get(uint64_t h){
    for (unsigned i = 0; i < PC.n; i++) if (PC.e[i].hash == h){
        GLuint p = glCreateProgram();
        glProgramBinary(p, PC.e[i].fmt, PC.data + PC.e[i].off, (GLsizei)PC.e[i].len);
        GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok) return p;
        glDeleteProgram(p); PC.failed++;
        return 0;
    }
    return 0;
}
static void pc_put(GLuint prog, uint64_t h){
    GLint len = 0; glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || len > (16 << 20)) return;
    unsigned char *b = malloc((size_t)len + 16); if (!b) return;
    GLsizei got = 0; GLenum fmt = 0;
    glGetProgramBinary(prog, len, &got, &fmt, b + 16);
    if (got <= 0){ free(b); return; }
    uint32_t l32 = (uint32_t)got, f32 = (uint32_t)fmt;
    memcpy(b, &h, 8); memcpy(b + 8, &f32, 4); memcpy(b + 12, &l32, 4);
    FILE *f = fopen(pc_path(), PC.fresh ? "wb" : "ab");
    if (f){
        if (PC.fresh){ fwrite("NPC1", 1, 4, f); fwrite(&PC.env, 1, 8, f); PC.fresh = 0; }
        fwrite(b, 1, (size_t)got + 16, f); fclose(f); PC.saved++;
    }
    free(b);
}
static uint64_t src_hash(const char *v, const char *f){ return fnv64(fnv64(14695981039346656037ull, v), f) ^ 0x9E3779B97F4A7C15ull; }

static Prog *get_prog(int vsid, int psid){
    uint32_t key = ((uint32_t)vsid << 12) | (uint32_t)psid;
    if (G.nprog * 2 >= G.capprog){
        unsigned nc = G.capprog ? G.capprog * 2 : 256; Prog *np = calloc(nc, sizeof *np);
        for (unsigned i = 0; i < G.capprog; i++) if (G.progs[i].key) { unsigned h = (G.progs[i].key * 2654435761u) % nc; while (np[h].key) h = (h + 1) % nc; np[h] = G.progs[i]; }
        free(G.progs); G.progs = np; G.capprog = nc;
    }
    unsigned h = ((key + 1) * 2654435761u) % G.capprog;
    while (G.progs[h].key){ if (G.progs[h].key == key + 1) return &G.progs[h]; h = (h + 1) % G.capprog; }
    Prog *p = &G.progs[h]; p->key = key + 1; G.nprog++;
    const char *v = vsid >= 0 && vsid < MAXVS ? G.vs_glsl[vsid] : NULL, *f = psid >= 0 && psid < MAXPS ? G.ps_glsl[psid] : NULL;
    if (!v || !f){ p->bad = 1; return p; }
    char what[48]; snprintf(what, sizeof what, "vs_%d+ps_%d", vsid, psid);
    G.phase = 4; glr_log("program %s: compiling", what);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t sh = PC.on ? src_hash(v, f) : 0;
    int cached = 0;
    if (PC.on){ p->prog = pc_get(sh); cached = p->prog != 0; }
    if (!p->prog) p->prog = make_prog(v, f, what);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    glr_log("program %s: %s%s in %.0f ms", what, p->prog ? "ok" : "FAILED", cached ? " (from the cache)" : "", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    if (p->prog && PC.on && !cached) pc_put(p->prog, sh);
    if (cached) PC.loaded++;
    if (!p->prog){ p->bad = 1; return p; }
    p->kb = glGetUniformLocation(p->prog, "u_kb"); p->af = glGetUniformLocation(p->prog, "ALPHA_FUNC");
    G.progs_made++;
    return p;
}

/* ---- textures ---- */
static GLuint make_white(int cube){
    GLuint t; glGenTextures(1, &t);
    unsigned char px[4] = { 255, 255, 255, 255 };
    if (!cube){ glBindTexture(GL_TEXTURE_2D, t); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); }
    else { glBindTexture(GL_TEXTURE_CUBE_MAP, t); for (int f = 0; f < 6; f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); }
    return t;
}
static GLenum internal_format(uint32_t fmt){
    switch (fmt){
    case 21: case 22: return GL_RGBA8;
    case 28: case 41: return GL_R8;
    case 0x31545844u: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    case 0x33545844u: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    case 0x35545844u: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    }
    return 0;
}
void glr_tex_free(uint32_t id){
    if (id >= MAXTEX || !G.tex[id].live) return;
    glDeleteTextures(1, &G.tex[id].gl); memset(&G.tex[id], 0, sizeof G.tex[id]);
}
void glr_tex_create(uint32_t id, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, uint32_t flags){
    if (id >= MAXTEX) return;
    glr_tex_free(id);
    GLenum gf = internal_format(fmt);
    if (!gf || !w || !h || w > 16384 || h > 16384) return;
    int cube = !!(flags & GW_TF_CUBE), rt = !!(flags & GW_TF_RT) && fmt != 28 && fmt != 41 && (fmt == 21 || fmt == 22);
    unsigned maxlv = 1; for (unsigned m = w > h ? w : h; m > 1; m >>= 1) maxlv++;
    if (levels < 1) levels = 1; if (levels > maxlv) levels = maxlv;
    Tex *t = &G.tex[id];
    GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glGenTextures(1, &t->gl); glBindTexture(target, t->gl);
    glTexStorage2D(target, (GLsizei)levels, gf, (GLsizei)w, (GLsizei)h);
    if (fmt == 28){ GLint sw[4] = { GL_ZERO, GL_ZERO, GL_ZERO, GL_RED }; glTexParameteriv(target, GL_TEXTURE_SWIZZLE_RGBA, sw); }
    else if (fmt == 22){ glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ONE); }
    t->fmt = fmt; t->w = w; t->h = h; t->levels = levels; t->cube = (uint8_t)cube; t->rt = (uint8_t)rt; t->live = 1;
    G.tex_made++;
}
void glr_tex_upload(uint32_t id, uint32_t level, uint32_t face, uint32_t lw, uint32_t lh, uint32_t pitch, uint32_t rows, const void *data){
    if (id >= MAXTEX || !G.tex[id].live || level >= G.tex[id].levels) return;
    Tex *t = &G.tex[id];
    GLenum target = t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, tt = t->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (face > 5 ? 0 : face) : GL_TEXTURE_2D;
    glBindTexture(target, t->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (t->fmt == 0x31545844u || t->fmt == 0x33545844u || t->fmt == 0x35545844u){
        unsigned bb = t->fmt == 0x31545844u ? 8 : 16, tight = ((lw + 3) / 4) * bb, br = (lh + 3) / 4;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        const void *p = data; unsigned char *tmp = NULL;
        if (pitch != tight){ tmp = malloc((size_t)tight * br); if (!tmp) return; for (unsigned y = 0; y < br && y < rows; y++) memcpy(tmp + (size_t)y * tight, (const unsigned char *)data + (size_t)y * pitch, tight); p = tmp; }
        glCompressedTexSubImage2D(tt, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh, internal_format(t->fmt), (GLsizei)(tight * br), p);
        free(tmp);
    } else if (t->fmt == 28 || t->fmt == 41){
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)pitch);
        glTexSubImage2D(tt, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh, GL_RED, GL_UNSIGNED_BYTE, data);
    } else {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 4));
        glTexSubImage2D(tt, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh, GL_BGRA, GL_UNSIGNED_BYTE, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    G.tex_up++;
}

/* ---- the persistent vertex/index pool ---- */
void glr_pool_grow(uint32_t size){
    size = ((size + 3u) & ~3u) + (1u << 20);                 /* slack past the end: an out-of-range pull must not fault the GPU */
    GLuint nb; glGenBuffers(1, &nb);
    glBindBuffer(GL_COPY_WRITE_BUFFER, nb); glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)size, NULL, GL_DYNAMIC_DRAW);
    if (G.pool_buf){
        glBindBuffer(GL_COPY_READ_BUFFER, G.pool_buf);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (GLsizeiptr)(G.pool_cap < size ? G.pool_cap : size));
        glDeleteBuffers(1, &G.pool_buf);
    }
    G.pool_buf = nb; G.pool_cap = size;
}
void glr_pool_write(uint32_t off, uint32_t n, const void *data){
    if (!G.pool_buf || (size_t)off + n > G.pool_cap) return;
    glBindBuffer(GL_COPY_WRITE_BUFFER, G.pool_buf); glBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)off, (GLsizeiptr)n, data);
}
void glr_z_forget(uint32_t zid){
    if (zid < 64 && G.z_rbo[zid]){ glDeleteRenderbuffers(1, &G.z_rbo[zid]); G.z_rbo[zid] = 0; G.z_w[zid] = G.z_h[zid] = 0; }
}

/* ---- samplers (the key layout of com.c smp_push, as encoder.ts sampler()) ---- */
static GLuint sampler_for(uint32_t k){
    for (unsigned i = 0; i < G.nsampler; i++) if (G.sampler_key[i] == k) return G.samplers[i];
    if (G.nsampler >= 4096) return G.samplers[0];
    unsigned mn = k & 3, mg = (k >> 2) & 3, mp = (k >> 4) & 3, au = (k >> 6) & 7, av = (k >> 9) & 7, al = (k >> 12) & 7;
    GLuint s; glGenSamplers(1, &s);
    int aniso = (mn == 3 || mg == 3) ? (1 << al > 16 ? 16 : 1 << al) : 1;
    GLint minf, magf = mg == 1 ? GL_NEAREST : GL_LINEAR;
    int linear_min = mn != 1, linear_mip = mp == 2;
    if (aniso > 1){ linear_min = 1; magf = GL_LINEAR; linear_mip = 1; }
    if (mp == 0) minf = linear_min ? GL_LINEAR : GL_NEAREST;
    else minf = linear_min ? (linear_mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : (linear_mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
    static const GLint addr[6] = { GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT };
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minf); glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, magf);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, addr[au < 6 ? au : 1]); glSamplerParameteri(s, GL_TEXTURE_WRAP_T, addr[av < 6 ? av : 1]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, 0.0f); glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, mp == 0 ? 0.0f : 1000.0f);
    if (aniso > 1) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)aniso);
    G.samplers[G.nsampler] = s; G.sampler_key[G.nsampler] = k; return G.samplers[G.nsampler++];
}

/* ---- state tables of encoder.ts ---- */
static GLenum blend_factor(unsigned i){
    static const GLenum t[16] = { GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
        GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR };
    return t[i & 15];
}
static GLenum depth_func(unsigned i){
    static const GLenum t[9] = { GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    return t[i > 8 ? 0 : i];
}

/* ---- init ---- */
static const char *PRESENT_VS =
    "#version 430 core\nout vec2 uv;\n"
    "void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *PRESENT_FS =
    "#version 430 core\nin vec2 uv; out vec4 o; layout(binding = 0) uniform sampler2D src; uniform int flip;\n"
    "void main(){ vec2 c = uv; if (flip != 0) c.y = 1.0 - c.y; o = vec4(texture(src, c).rgb, 1.0); }\n";

int glr_init(void *native_window, int w, int h){
    { const char *tb = getenv("NFSU2_NX_TBUF"); G.tbuf = tb && *tb == '1'; if (G.tbuf) glr_log("vertex buffer read as a buffer texture (NFSU2_NX_TBUF)"); }
#ifndef GLR_CONSOLE
    G.dpy = EGL_NO_DISPLAY;
    { PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
      if (gpd) G.dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
      glr_log("platform display %p (proc %p, egl error 0x%x)", (void *)G.dpy, (void *)(uintptr_t)gpd, eglGetError()); }
    if (G.dpy == EGL_NO_DISPLAY) G.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
    G.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
    if (G.dpy == EGL_NO_DISPLAY || !eglInitialize(G.dpy, NULL, NULL)){ glr_log("eglInitialize failed 0x%x", eglGetError()); return -1; }
    if (!eglBindAPI(EGL_OPENGL_API)){ glr_log("eglBindAPI(OpenGL) failed"); return -2; }
    EGLConfig cfg; EGLint n = 0;
    EGLint ca_win[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLint ca_pb[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    eglChooseConfig(G.dpy, native_window ? ca_win : ca_pb, &cfg, 1, &n);
    if (!n){ glr_log("no EGL config"); return -3; }
    if (native_window){ G.srf = eglCreateWindowSurface(G.dpy, cfg, (EGLNativeWindowType)native_window, NULL); G.onscreen = 1; }
    else { EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE }; G.srf = eglCreatePbufferSurface(G.dpy, cfg, pa); }
    if (G.srf == EGL_NO_SURFACE){ glr_log("no surface 0x%x", eglGetError()); return -4; }
    EGLint xa[] = { EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE };
    G.ctx = eglCreateContext(G.dpy, cfg, EGL_NO_CONTEXT, xa);
    if (G.ctx == EGL_NO_CONTEXT){ glr_log("no GL 4.3 core context 0x%x", eglGetError()); return -5; }
    eglMakeCurrent(G.dpy, G.srf, G.srf, G.ctx);
    eglSwapInterval(G.dpy, 1);
#ifdef GLR_CONSOLE
    if (!gladLoadGL()){ glr_log("gladLoadGL failed"); return -6; }
#endif
    EGLint ww = w, hh = h; eglQuerySurface(G.dpy, G.srf, EGL_WIDTH, &ww); eglQuerySurface(G.dpy, G.srf, EGL_HEIGHT, &hh);
    if (ww <= 0 || hh <= 0){ glr_log("the window reports %dx%d: using 1280x720", ww, hh); ww = 1280; hh = 720; }
    G.win_w = ww; G.win_h = hh;
    glr_log("GL %s / %s, window %dx%d", glGetString(GL_VERSION), glGetString(GL_RENDERER), ww, hh);
    pc_init();
    p_clip_control = (void (*)(GLenum, GLenum))(void *)eglGetProcAddress("glClipControl");
    if (!p_clip_control) glr_log("glClipControl is not available: the picture will be upside down");
    glGenVertexArrays(1, &G.vao); glBindVertexArray(G.vao);
    glGenFramebuffers(1, &G.fbo); glGenFramebuffers(1, &G.fbo_read); glGenFramebuffers(1, &G.fbo_draw);
    for (int q = 0; q < 3; q++){ glGenBuffers(1, &G.tv_buf[q]); glGenBuffers(1, &G.raw_buf[q]); glGenBuffers(1, &G.k_buf[q]); glGenBuffers(1, &G.p_buf[q]); }
    G.white = make_white(0); G.white_cube = make_white(1);
    G.present_prog = make_prog(PRESENT_VS, PRESENT_FS, "present");
    G.present_flip = glGetUniformLocation(G.present_prog, "flip");
    ov_init();
    G.touched = calloc((size_t)MAXTEX * 8, 1); G.touched_list = malloc(sizeof(unsigned) * 4096);
    G.sampler_key[0] = 0xFFFFFFFFu; G.samplers[0] = 0; sampler_for(0x0F); G.nsampler = G.nsampler;
    return 0;
}

void glr_want_shots(unsigned every){ G.shot_every = every; }
const uint32_t *glr_shot(unsigned *w, unsigned *h, unsigned *seq){
    *w = G.shot_w; *h = G.shot_h; *seq = G.shot_seq;
    return G.shot_seq ? G.shot[(G.shot_seq - 1) & 1u] : NULL;
}
void glr_stats(char *buf, unsigned n){
    double fr = G.frames > G.frames_at_report ? (double)(G.frames - G.frames_at_report) : 1.0;     /* the averages are since the last report */
    char prof[160]; { double k = 1000.0 / TICK_HZ / fr; snprintf(prof, sizeof prof, " prof(ms/frame): prog %.1f state %.1f bufbind %.1f tex %.1f draw %.1f pre %.1f", G.pf[0] * k, G.pf[1] * k, G.pf[2] * k, G.pf[3] * k, G.pf[4] * k, G.pf[5] * k); }
    snprintf(buf, n, "frames %lu draws %lu skipped %lu passes %lu programs %lu shader-fail %lu textures %lu uploads %lu | per frame: record %.2f ms, swap %.2f ms%s",
             G.frames, G.draws, G.skipped, G.passes, G.progs_made, G.shader_fail, G.tex_made, G.tex_up, G.ms_build / fr, G.ms_swap / fr, prof);
    G.frames_at_report = G.frames; G.ms_build = G.ms_swap = 0; memset(G.pf, 0, sizeof G.pf);
}

/* ---- the frame ---- */
static void ensure_buf(GLuint buf, GLenum target, size_t *cap, size_t need){
    need += 65536;                                         /* slack: a pull past the data reads zeros, not unmapped memory */
    if (*cap >= need) return;
    size_t nc = (need * 5 / 4 + 262143) & ~(size_t)262143;
    glBindBuffer(target, buf); glBufferData(target, (GLsizeiptr)nc, NULL, GL_STREAM_DRAW); *cap = nc;
}
static void ensure_bb(unsigned w, unsigned h){
    if (G.bb && G.bb_w == w && G.bb_h == h) return;
    if (G.bb) glDeleteTextures(1, &G.bb);
    glGenTextures(1, &G.bb); glBindTexture(GL_TEXTURE_2D, G.bb);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G.bb_w = w; G.bb_h = h;
    glr_log("back buffer %ux%u", w, h);
}
static void put_touched(unsigned key){ if (!G.touched[key]){ G.touched[key] = 1; if (G.ntouched < 4096) G.touched_list[G.ntouched++] = key; } }

/* ---- the fps counter: "28 FPS 35 MS" in the top left corner, the text drawn on the CPU into a small texture once a second ---- */
static const unsigned char k_font[][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2},   /* 0 1 2 3 4 */
    {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12}, /* 5 6 7 8 9 */
    {31,16,16,30,16,16,16}, {30,17,17,30,16,16,16}, {15,16,16,14,1,1,30}, {17,27,21,21,17,17,17},             /* F P S M */
};
static int font_idx(char c){
    if (c >= '0' && c <= '9') return c - '0';
    return c == 'F' ? 10 : c == 'P' ? 11 : c == 'S' ? 12 : c == 'M' ? 13 : -1;
}
#define OV_SC 3
#define OV_W 288
#define OV_H 30
static void ov_text(const char *txt){
    static uint32_t px[OV_W * OV_H];
    for (int i = 0; i < OV_W * OV_H; i++) px[i] = 0xA0000000u;                       /* a dark box, mostly opaque (ABGR: alpha first in the top byte) */
    int x0 = 6;
    for (const char *c = txt; *c; c++, x0 += 6 * OV_SC){
        int gi = font_idx(*c); if (gi < 0) continue;
        for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++) if (k_font[gi][r] & (16 >> b))
            for (int dy = 0; dy < OV_SC; dy++) for (int dx = 0; dx < OV_SC; dx++){
                int X = x0 + b * OV_SC + dx, Y = 4 + r * OV_SC + dy;
                if (X < OV_W && Y < OV_H) px[Y * OV_W + X] = 0xFFFFFFFFu;
            }
    }
    glBindTexture(GL_TEXTURE_2D, G.ov_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, OV_W, OV_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
}
static const char *OV_VS = "#version 430 core\nuniform vec4 u_rect;\nout vec2 v_uv;\nvoid main(){ vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)); gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0); v_uv = vec2(c.x, 1.0 - c.y); }\n";
static const char *OV_FS = "#version 430 core\nuniform sampler2D u_tex;\nin vec2 v_uv;\nout vec4 o;\nvoid main(){ o = texture(u_tex, v_uv); }\n";
static void ov_init(void){
    G.ov_prog = make_prog(OV_VS, OV_FS, "overlay");
    if (!G.ov_prog) return;
    G.ov_rect = glGetUniformLocation(G.ov_prog, "u_rect");
    glGenTextures(1, &G.ov_tex); glBindTexture(GL_TEXTURE_2D, G.ov_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, OV_W, OV_H);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ov_text("0 FPS");
    G.ov_on = 1;                                                                    /* on until the player turns it off (L3 + R3) */
}
void glr_overlay_toggle(void){ G.ov_on = !G.ov_on; }
/* once per frame, over the finished picture: counts, rewrites the text every second, draws the quad */
static void ov_draw(void){
    if (!G.ov_prog) return;
    uint64_t now = tick();
    if (!G.ov_t0) G.ov_t0 = now;
    G.ov_frames++;
    if ((double)(now - G.ov_t0) / TICK_HZ >= 1.0){
        double secs = (double)(now - G.ov_t0) / TICK_HZ;
        double fps = G.ov_frames / secs;
        char t[40]; snprintf(t, sizeof t, "%d FPS %d MS", (int)(fps + 0.5), fps > 0.5 ? (int)(1000.0 / fps + 0.5) : 0);
        ov_text(t);
        G.ov_t0 = now; G.ov_frames = 0;
    }
    if (!G.ov_on) return;
    float W = (float)G.win_w, H = (float)G.win_h, px = 16.0f, py = 16.0f;
    float x0 = -1.0f + 2.0f * px / W, x1 = -1.0f + 2.0f * (px + OV_W) / W;
    float y1 = 1.0f - 2.0f * py / H, y0 = 1.0f - 2.0f * (py + OV_H) / H;
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(G.ov_prog); glUniform4f(G.ov_rect, x0, y0, x1, y1);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, G.ov_tex); glBindSampler(0, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

void glr_frame(const GwFrame *F, const GwDraw *dr, const uint8_t *tv, const uint8_t *raw, const uint8_t *kdat, const float *psc){
    unsigned dn = F->dn, vn = F->vn, rvn = F->rvn, kn = F->kn;
    struct timespec tf0; clock_gettime(CLOCK_MONOTONIC, &tf0);
    const unsigned S = (unsigned)(G.frames % 3);
    G.frame_no++; G.phase = 1;
    unsigned bbw = F->bb_w ? F->bb_w : 1280, bbh = F->bb_h ? F->bb_h : 720;
    ensure_bb(bbw, bbh);
    glDisable(GL_SCISSOR_TEST);
    clip_control(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
    glBindVertexArray(G.vao);
    if (vn){ ensure_buf(G.tv_buf[S], GL_SHADER_STORAGE_BUFFER, &G.tv_cap[S], (size_t)vn * TVERT); glBindBuffer(GL_SHADER_STORAGE_BUFFER, G.tv_buf[S]); glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)vn * TVERT, tv); }
    if (rvn){ size_t n = (rvn + 3u) & ~3u; ensure_buf(G.raw_buf[S], GL_SHADER_STORAGE_BUFFER, &G.raw_cap[S], n); glBindBuffer(GL_SHADER_STORAGE_BUFFER, G.raw_buf[S]); glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)n, raw); }
    { size_t n = (kn + 3u) & ~3u; ensure_buf(G.k_buf[S], GL_UNIFORM_BUFFER, &G.k_cap[S], n + KWIN); if (kn){ glBindBuffer(GL_UNIFORM_BUFFER, G.k_buf[S]); glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)n, kdat); } }
    /* the PS parameter slots, as the page builds them */
    if (G.pstage_cap < (size_t)(dn + 1) * (PSLOT / 4)){ free(G.pstage); G.pstage_cap = (size_t)(dn + 64) * (PSLOT / 4); G.pstage = malloc(G.pstage_cap * sizeof(float)); }
    float *ps = G.pstage; memset(ps, 0, (size_t)(dn + 1) * PSLOT);
    unsigned pslots = 0; int lastPsci = -1; float lastAref = NAN, lastPF[8] = { 0 };
    ensure_buf(G.p_buf[S], GL_UNIFORM_BUFFER, &G.p_cap[S], (size_t)(dn + 1) * PSLOT);
    /* every draw's slot first, and one upload for all of them: a glBufferSubData per slot (hundreds a frame) was most of the
     * 40-55 ms the driver took to record a race frame on the console */
    if (G.pslot_cap < dn + 1){ free(G.pslot_of); G.pslot_cap = dn + 256; G.pslot_of = malloc(G.pslot_cap * sizeof(uint32_t)); }
    uint64_t kp0 = tick();
    for (unsigned i = 0; i < dn; i++){
        const GwDraw *D = &dr[i];
        if (D->kind == GW_BLIT || D->kind == GW_CLEAR) continue;
        float aref = D->aref; int psci = (int)D->psci; int same = psci == lastPsci && aref == lastAref;
        if (same) for (int q = 0; q < 4; q++) if (lastPF[q] != D->proj[q] || lastPF[4 + q] != D->fogv[q]){ same = 0; break; }
        if (!same){
            float *o = ps + (size_t)pslots * (PSLOT / 4);
            o[0] = aref;
            for (int q = 0; q < 4; q++){ o[4 + q] = lastPF[q] = D->proj[q]; o[136 + q] = lastPF[4 + q] = D->fogv[q]; }
            memcpy(o + 8, psc + (size_t)psci * 128, 128 * sizeof(float));
            lastPsci = psci; lastAref = aref; pslots++;
        }
        G.pslot_of[i] = pslots - 1;
    }
    if (pslots){ glBindBuffer(GL_UNIFORM_BUFFER, G.p_buf[S]); glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)pslots * PSLOT, ps); }
    G.pf[5] += tick() - kp0;

    int curT = -1; int curZ = -1, realZ = 0; unsigned passW = 0, passH = 0; int passCube = -1;
    int inpass = 0, screenTouched = 0;
    unsigned clearArgb = F->clear_argb;
    uint32_t cSrc = 0xFFFFFFFFu, cK = 0xFFFFFFFFu, cP = 0xFFFFFFFFu, cBcol = 0xFFFFFFFFu, cClip = 0xFFFFFFFFu;
    GLuint curProg = 0; uint32_t cBlend = 0xFFFFFFFFu, cDepth = 0xFFFFFFFFu, cCull = 0xFFFFFFFFu;
    GLuint ctex[4] = { 0 }; GLenum ctgt[4] = { 0 }; uint32_t csmp[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    int cIx = 0;
    G.ntouched = 0;
    unsigned draws = 0, skipped = 0;
    for (unsigned i = 0; i < dn; i++){
        const GwDraw *D = &dr[i];
        if (D->kind == GW_BLIT){
            uint32_t sid = D->blit_src, tgt = D->target;
            Tex *st = sid && sid < MAXTEX && G.tex[sid].live ? &G.tex[sid] : NULL, *dt = tgt && (tgt & 0xFFFF) < MAXTEX && G.tex[tgt & 0xFFFF].live ? &G.tex[tgt & 0xFFFF] : NULL;
            if ((sid && !st) || (tgt && (!dt || !dt->rt)) || (sid && sid == (tgt & 0xFFFF))){ skipped++; continue; }
            unsigned sw = sid ? st->w : bbw, sh = sid ? st->h : bbh, dw = tgt ? dt->w : bbw, dh = tgt ? dt->h : bbh;
            int sx0 = 0, sy0 = 0, sx1 = (int)sw, sy1 = (int)sh;
            if (D->count){ sx0 = (int)(D->first & 0xFFFF); sy0 = (int)(D->first >> 16); sx1 = (int)(D->count & 0xFFFF); sy1 = (int)(D->count >> 16); if (sx1 > (int)sw) sx1 = (int)sw; if (sy1 > (int)sh) sy1 = (int)sh; }
            if (sx1 <= sx0 || sy1 <= sy0){ skipped++; continue; }
            glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, G.fbo_read); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, G.fbo_draw);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sid ? (st->cube ? 0 : st->gl) : G.bb, 0);
            if (sid && st->cube) glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X, st->gl, 0);
            unsigned face = (tgt >> 16) & 7;
            if (tgt && dt->cube) glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + (face ? (face > 6 ? 5 : face - 1) : 0), dt->gl, 0);
            else glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tgt ? dt->gl : G.bb, 0);
            int same = (sx1 - sx0) == (int)dw && (sy1 - sy0) == (int)dh;
            glBlitFramebuffer(sx0, sy0, sx1, sy1, 0, 0, (GLint)dw, (GLint)dh, GL_COLOR_BUFFER_BIT, same ? GL_NEAREST : GL_LINEAR);
            if (tgt) put_touched((tgt & 0xFFFF) * 8 + ((tgt >> 16) & 7)); else screenTouched = 1;
            inpass = 0; curT = -1; curZ = -1; curProg = 0; cSrc = cK = cP = 0xFFFFFFFFu; cBlend = cDepth = cCull = cClip = cBcol = 0xFFFFFFFFu;
            continue;
        }
        uint32_t depth = D->depth, zid = depth >> 8, target = D->target;
        if (D->kind == GW_CLEAR){
            if (D->clr_flags & 1u) clearArgb = D->clr_argb;
            if ((D->clr_flags & 2u) && zid && zid < 64){
                G.zpend[zid] = 1; G.zpend_v[zid] = D->clr_z < 0 ? 0 : D->clr_z > 1 ? 1 : D->clr_z;
                if (inpass && curZ == (int)zid){ inpass = 0; curT = -1; }
            }
        }
        int zsplit = (int)zid != curZ && !(inpass && D->kind != GW_CLEAR && (depth & 3) == 0);
        if (curT == -1 || (int)target != curT || zsplit){
            inpass = 0; curT = (int)target; curZ = (int)zid;
            /* open the pass: the target and its Z surface, a clear on the target's first pass of the frame */
            Tex *t = target ? &G.tex[target & 0xFFFF] : NULL;
            unsigned face = (target >> 16) & 7;
            if (target && (!t || !t->live || !t->rt)){ inpass = 0; }
            else {
                unsigned w, h; unsigned key;
                if (!target){ w = bbw; h = bbh; key = MAXTEX * 8 - 1; passCube = -1; }
                else { w = t->w; h = t->h; key = (target & 0xFFFF) * 8 + face; passCube = (int)(target & 0xFFFF); }
                passW = w; passH = h;
                int first = !target ? !screenTouched : !G.touched[key];
                glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
                if (!target) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.bb, 0);
                else if (t->cube) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + (face ? (face > 6 ? 5 : face - 1) : 0), t->gl, 0);
                else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->gl, 0);
                int zclear = 0; float zval = 1.0f; realZ = 0; GLuint zr = 0;
                if (zid && zid < 64){
                    if (!G.z_rbo[zid] || G.z_w[zid] != w || G.z_h[zid] != h){
                        if (!G.z_rbo[zid] || zid == 1){
                            if (G.z_rbo[zid]) glDeleteRenderbuffers(1, &G.z_rbo[zid]);
                            glGenRenderbuffers(1, &G.z_rbo[zid]); glBindRenderbuffer(GL_RENDERBUFFER, G.z_rbo[zid]);
                            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, (GLsizei)w, (GLsizei)h);
                            G.z_w[zid] = w; G.z_h[zid] = h; zclear = 1;
                        } else zr = 0;
                    }
                    if (G.z_rbo[zid] && G.z_w[zid] == w && G.z_h[zid] == h){
                        zr = G.z_rbo[zid]; realZ = 1;
                        if (G.zpend[zid]){ zclear = 1; zval = G.zpend_v[zid]; G.zpend[zid] = 0; }
                    }
                }
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zr);
                glViewport(0, 0, (GLsizei)w, (GLsizei)h);
                glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1); glDepthMask(GL_TRUE);
                if (first){ float c[4] = { ((clearArgb >> 16) & 255) / 255.0f, ((clearArgb >> 8) & 255) / 255.0f, (clearArgb & 255) / 255.0f, 1.0f }; glClearBufferfv(GL_COLOR, 0, c); }
                if (realZ && zclear) glClearBufferfv(GL_DEPTH, 0, &zval);
                if (!target) screenTouched = 1; else put_touched(key);
                inpass = 1; G.passes++;
                cSrc = cK = cP = 0xFFFFFFFFu; cBcol = 0xFFFFFFFFu; cClip = 0xFFFFFFFFu; cBlend = cDepth = cCull = 0xFFFFFFFFu; curProg = 0;
                for (int q = 0; q < 4; q++) csmp[q] = 0xFFFFFFFFu, ctex[q] = 0;
                glBindBufferRange(GL_UNIFORM_BUFFER, 3, G.k_buf[S], GW_K_HL, GW_HL_BYTES);
            }
        }
        if (D->kind == GW_CLEAR) continue;
        unsigned count = D->count;
        if (!inpass || !count){ skipped++; continue; }

        /* program */
        int psid = D->ps, vsid = D->vsid;
        if (vsid > 0 && !(vsid < MAXVS && G.vs_glsl[vsid])){ skipped++; continue; }
        float aref = D->aref;
        uint32_t t0 = D->tex;
        int psEff = (psid > 0 && psid < MAXPS && G.ps_glsl[psid]) ? psid : (psid >= PSCUBE ? PSCUBE : 0);
        if (psEff == 0 && t0 && t0 < MAXTEX && G.tex[t0].live && G.tex[t0].cube) psEff = PSCUBE;
        G.phase = 3; G.ph_a = i; G.ph_b = (unsigned)vsid; G.ph_c = (unsigned)psEff;
        uint64_t k0 = tick();
        Prog *pg = get_prog(vsid, psEff);
        if (pg->bad){ skipped++; continue; }
        if (pg->prog != curProg){ glUseProgram(pg->prog); curProg = pg->prog; }
        { int af = aref > 0.0f ? 7 : 8; if (pg->last_af != af + 1){ glUniform1ui(pg->af, (unsigned)af); pg->last_af = af + 1; } }

        uint64_t k1 = tick(); G.pf[0] += k1 - k0;
        /* state */
        uint32_t blend = D->blend & 0x3FFFFu, cull = D->cull & 3u, dm = realZ ? (depth & 63u) : 0u;
        if (blend != cBlend){
            unsigned en = blend & 1u, sf = (blend >> 1) & 31u, df = (blend >> 6) & 31u, op = (blend >> 11) & 7u, mask = (blend >> 14) & 15u;
            if (en){
                GLenum eq = op == 2 ? GL_FUNC_SUBTRACT : op == 3 ? GL_FUNC_REVERSE_SUBTRACT : op == 4 ? GL_MIN : op == 5 ? GL_MAX : GL_FUNC_ADD;
                GLenum s = blend_factor(sf), d = blend_factor(df);
                if (sf == 12) d = GL_ONE_MINUS_SRC_ALPHA;
                if (sf == 13){ s = GL_ONE_MINUS_SRC_ALPHA; d = GL_SRC_ALPHA; }
                if (eq == GL_MIN || eq == GL_MAX){ s = GL_ONE; d = GL_ONE; }
                glEnable(GL_BLEND); glBlendEquation(eq); glBlendFuncSeparate(s, d, s, d);
            } else glDisable(GL_BLEND);
            glColorMask((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0);
            cBlend = blend;
        }
        if (dm != cDepth){
            if (realZ){ glEnable(GL_DEPTH_TEST); glDepthFunc((dm & 1) ? depth_func((dm >> 2) & 7) : GL_ALWAYS); glDepthMask((dm & 2) != 0); }
            else glDisable(GL_DEPTH_TEST);
            cDepth = dm;
        }
        if (cull != cCull){
            glFrontFace(GL_CW);
            if (cull == 1) glDisable(GL_CULL_FACE); else { glEnable(GL_CULL_FACE); glCullFace(cull == 2 ? GL_FRONT : GL_BACK); }
            cCull = cull;
        }
        uint32_t clip = (passW == bbw && passH == bbh) ? D->clip : 0;
        if (clip != cClip){
            if (clip){ unsigned y0 = clip & 0xFFFF, y1 = clip >> 16; if (y0 > passH) y0 = passH; if (y1 > passH) y1 = passH; glEnable(GL_SCISSOR_TEST); glScissor(0, (GLint)(passH - y1), (GLsizei)passW, (GLsizei)(y1 > y0 ? y1 - y0 : 0)); }
            else glDisable(GL_SCISSOR_TEST);
            cClip = clip;
        }
        if (D->bcol != cBcol){ uint32_t b = D->bcol; glBlendColor(((b >> 16) & 255) / 255.0f, ((b >> 8) & 255) / 255.0f, (b & 255) / 255.0f, (b >> 24) / 255.0f); cBcol = b; }

        uint64_t k2 = tick(); G.pf[1] += k2 - k1;
        uint32_t poff = G.pslot_of[i] * PSLOT;

        /* group 0 */
        uint32_t isz = D->ix_isz;
        uint32_t src = isz ? 2u : vsid > 0 ? 1u : 0u;
        int koff = vsid > 0 ? (int)D->koff : -1;
        if (src != cSrc){
            GLuint vb = src == 0 ? G.tv_buf[S] : src == 1 ? G.raw_buf[S] : G.pool_buf;
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, vb);
            if (G.tbuf){ glActiveTexture(GL_TEXTURE0 + 12); if (!G.tb_tex) glGenTextures(1, &G.tb_tex); glBindTexture(GL_TEXTURE_BUFFER, G.tb_tex); glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, vb); }
            cSrc = src;
        }
        if (koff >= 0 && (cK == 0xFFFFFFFFu || (uint32_t)koff < cK || (uint32_t)koff - cK > KWIN - (16u + 256u * 16u))){
            cK = (uint32_t)koff & ~255u;
            glBindBufferRange(GL_UNIFORM_BUFFER, 1, G.k_buf[S], (GLintptr)cK, KWIN);
        } else if (cK == 0xFFFFFFFFu){ cK = 0; glBindBufferRange(GL_UNIFORM_BUFFER, 1, G.k_buf[S], 0, KWIN); }
        if (poff != cP){ glBindBufferRange(GL_UNIFORM_BUFFER, 2, G.p_buf[S], (GLintptr)poff, 560); cP = poff; }
        int kb = koff >= 0 ? (koff - (int)cK) >> 4 : 0;
        if (pg->last_kb != kb + 1){ glUniform1i(pg->kb, kb); pg->last_kb = kb + 1; }

        uint64_t k3 = tick(); G.pf[2] += k3 - k2;
        /* group 1: textures and samplers */
        int cube = psEff >= PSCUBE;
        uint32_t ids[4] = { t0, D->tex1, D->tex2, D->tex3 };
        for (int u = 0; u < 4; u++){
            GLenum tgt = (u == 0 && cube) ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
            GLuint name;
            uint32_t id = ids[u];
            Tex *t = id && id < MAXTEX && G.tex[id].live ? &G.tex[id] : NULL;
            int want_cube = (u == 0 && cube);
            if (!t || (t->cube != 0) != want_cube || (int)id == passCube) name = want_cube ? G.white_cube : G.white; else name = t->gl;
            if (ctex[u] != name || ctgt[u] != tgt){ glActiveTexture(GL_TEXTURE0 + u); glBindTexture(tgt, name); ctex[u] = name; ctgt[u] = tgt; }
            if (csmp[u] != D->smp[u]){ glBindSampler(u, sampler_for(D->smp[u])); csmp[u] = D->smp[u]; }
        }

        uint64_t k4 = tick(); G.pf[3] += k4 - k3;
        if (isz){
            if (!cIx){ glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.pool_buf); cIx = 1; }
            glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)count, isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void *)(uintptr_t)D->ix_ib, (GLint)D->ix_base);
        } else if (vsid > 0) glDrawArrays(GL_TRIANGLES, 0, (GLsizei)count);
        else glDrawArrays(GL_TRIANGLES, (GLint)D->first, (GLsizei)count);
        G.pf[4] += tick() - k4;
        draws++;
    }
    for (unsigned q = 0; q < G.ntouched; q++) G.touched[G.touched_list[q]] = 0;
    if (!screenTouched){
        glBindFramebuffer(GL_FRAMEBUFFER, G.fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.bb, 0);
        glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
        float c[4] = { ((clearArgb >> 16) & 255) / 255.0f, ((clearArgb >> 8) & 255) / 255.0f, (clearArgb & 255) / 255.0f, 1.0f }; glClearBufferfv(GL_COLOR, 0, c);
    }
    G.draws += draws; G.skipped += skipped; G.frames++;

    /* present: the back buffer over the window */
    G.phase = 5;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    clip_control(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glColorMask(1, 1, 1, 1);
    glViewport(0, 0, G.win_w, G.win_h);
    glUseProgram(G.present_prog); glUniform1i(G.present_flip, 1);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, G.bb); glBindSampler(0, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ov_draw();
    if (G.shot_every && G.frames % G.shot_every == 0){
        unsigned w = (unsigned)G.win_w, h = (unsigned)G.win_h;
        if (G.shot_w != w || G.shot_h != h){ free(G.shot[0]); free(G.shot[1]); free(G.rb); G.shot[0] = malloc((size_t)w * h * 4); G.shot[1] = malloc((size_t)w * h * 4); G.rb = malloc((size_t)w * h * 4); G.shot_w = w; G.shot_h = h; }
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, G.rb);
        uint32_t *o = G.shot[G.shot_seq & 1u];
        for (unsigned y = 0; y < h; y++){ const unsigned char *s = G.rb + (size_t)(h - 1 - y) * w * 4; for (unsigned x = 0; x < w; x++, s += 4) o[(size_t)y * w + x] = 0xFF000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2]; }
        G.shot_seq++;
    }
    struct timespec tf1, tf2; clock_gettime(CLOCK_MONOTONIC, &tf1);
    G.phase = 6;
    eglSwapBuffers(G.dpy, G.srf);
    clock_gettime(CLOCK_MONOTONIC, &tf2);
    G.ms_build += (tf1.tv_sec - tf0.tv_sec) * 1e3 + (tf1.tv_nsec - tf0.tv_nsec) / 1e6;
    G.ms_swap += (tf2.tv_sec - tf1.tv_sec) * 1e3 + (tf2.tv_nsec - tf1.tv_nsec) / 1e6;
    glUseProgram(0);
    G.phase = 0;
}

unsigned long glr_frame_count(void){ return G.frame_no; }
void glr_where(char *buf, unsigned n){
    static const char *names[] = { "idle", "uploading arenas", "-", "drawing", "compiling a program", "presenting", "swapping buffers" };
    snprintf(buf, n, "%s (frame %lu, draw %u, vs %u, ps %u)", G.phase < 7 ? names[G.phase] : "?", G.frame_no, G.ph_a, G.ph_b, G.ph_c);
}

/* the context is released on the thread that owns it, before the window goes away with the process */
void glr_shutdown(void){
    if (!G.dpy) return;
    glFinish();
    eglMakeCurrent(G.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (G.ctx) eglDestroyContext(G.dpy, G.ctx);
    if (G.srf) eglDestroySurface(G.dpy, G.srf);
    eglTerminate(G.dpy);
    G.dpy = EGL_NO_DISPLAY; G.ctx = EGL_NO_CONTEXT; G.srf = EGL_NO_SURFACE;
    glr_log("shut down");
}

unsigned long nfsu2_gpu_shader_fails(void){ return G.shader_fail; }

void nfsu2_gpu_cache_report(FILE *f){
    fprintf(f, "program cache: %s; %u programs loaded from it, %u saved, %u that would not load\n", PC.on ? "on" : "off", PC.loaded, PC.saved, PC.failed);
}
