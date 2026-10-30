/* The wasm side of the page contract: shared structures the C runtime
 * publishes to the page with nfsu2_register(kind, ptr, size).  The page side is
 * web/src/host/abi.ts (same numbers).  One kind per subsystem; a subsystem
 * keeps its own layout header (gpu_proto.h, audio_proto.h, ...). */
#ifndef NFSU2_HOST_ABI_H
#define NFSU2_HOST_ABI_H
#include <stdint.h>
enum {
    HOST_KIND_GPU    = 1,   /* renderer control block (runtime/web/gpu_web.c) */
    HOST_KIND_AUDIO  = 2,   /* PCM ring + audio control (audio_web.c) */
    HOST_KIND_INPUT  = 3,   /* HostMailbox: keys, mouse, pad, lifecycle (input_web.c) */
    HOST_KIND_NET    = 4,   /* virtual LAN rings (net track) */
    HOST_KIND_STATUS = 5,   /* frame/fps/timing counters for the page overlay */
    HOST_KIND_VSYNC  = 6,   /* vsync clock the page fills from requestAnimationFrame */
    HOST_KIND_LAN    = 7,   /* LAN play's virtual-LAN rings (net_lan.c; layout net_proto.h) */
};
/* imported from the page (web/src/host/game-worker.ts, module "nfsu2") */
__attribute__((import_module("nfsu2"), import_name("register")))
int nfsu2_register(int kind, void *ptr, uint32_t size);
__attribute__((import_module("nfsu2"), import_name("now_ms")))
double nfsu2_now_ms(void);
/* ms the calling thread has spent blocked in the host (file I/O, sleeps) */
__attribute__((import_module("nfsu2"), import_name("blocked_ms")))
double nfsu2_blocked_ms(void);
/* the system's wall clock (Date.now(), smoothed below the millisecond), ms since 1970: what every page on the
 * machine reads alike, also after a step of the system clock (now_ms is the page's own, from when it was made) */
__attribute__((import_module("nfsu2"), import_name("wall_ms")))
double nfsu2_wall_ms(void);
#endif
