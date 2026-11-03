/* Switch: no dynamic loader; dlsym finds nothing (d3dx_hle.c's thread_selfcounts diagnostics stay off) */
#pragma once
#define RTLD_DEFAULT ((void *)0)
static inline void *dlsym(void *h, const char *n){ (void)h; (void)n; return 0; }
