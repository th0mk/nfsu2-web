/* gpu_gl.h -- the OpenGL 4.3 core renderer for the record buffers of runtime/web/gpu_web.c (the contract of
 * runtime/web/gpu_proto.h).  A port of web/src/gpu/encoder.ts (the WebGPU encoder): the same passes, the same state tables.
 * Everything here runs on one thread: the one that called glr_init (it owns the GL context). */
#ifndef NFSU2_GPU_GL_H
#define NFSU2_GPU_GL_H
#include <stdint.h>
#include "web/gpu_proto.h"

/* native_window: the Switch's NWindow, or NULL for an off-screen surface of w x h (the PC test).  0 on success. */
int  glr_init(void *native_window, int w, int h);
void glr_tex_create(uint32_t id, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, uint32_t flags);
void glr_tex_upload(uint32_t id, uint32_t level, uint32_t face, uint32_t lw, uint32_t lh, uint32_t pitch, uint32_t rows, const void *data);
void glr_tex_free(uint32_t id);
void glr_pool_grow(uint32_t size);
void glr_pool_write(uint32_t off, uint32_t n, const void *data);
void glr_shader(int is_ps, uint32_t id, const char *wgsl, uint32_t len);
void glr_z_forget(uint32_t zid);
/* draws one frame into the back buffer and presents it */
void glr_frame(const GwFrame *F, const GwDraw *draws, const uint8_t *tv, const uint8_t *raw, const uint8_t *k, const float *psc);
/* the last presented picture (ARGB, top row first) for the PC test; NULL until one exists (only when asked: glr_want_shots) */
void glr_want_shots(unsigned every);
const uint32_t *glr_shot(unsigned *w, unsigned *h, unsigned *seq);
void glr_stats(char *buf, unsigned n);
void glr_shutdown(void);
void glr_overlay_toggle(void);                    /* the fps counter on or off */
unsigned long glr_frame_count(void);              /* frames started: the monitor thread watches it */
void glr_where(char *buf, unsigned n);            /* what the renderer is doing right now */
#endif
