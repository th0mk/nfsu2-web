/* wgsl2glsl.h -- the WGSL modules the game's translator emits (docs/web/gpu-abi.md) as GLSL 4.30 core for the Switch's
 * OpenGL renderer.  A translator for the subset those modules use (structs, resources, functions, let/var, if, for,
 * switch, the vec/mat/scalar types and the builtins they call), with the type inference GLSL's explicit declarations need.
 * Resource bindings keep their WGSL numbers: storage buffer 0 = VB, uniform buffers 1 = K, 2 = P, 3 = H, texture units
 * 0..3 = tex0..tex3.  The draw's constant block row (WGSL's instance_index) is the uniform `u_kb`. */
#ifndef NFSU2_WGSL2GLSL_H
#define NFSU2_WGSL2GLSL_H
#include <stddef.h>

/* 0: ok (out holds the GLSL, NUL-terminated), else a negative code and err says why.  *is_fragment: 1 for a fragment module. */
int wgsl_to_glsl(const char *wgsl, char *out, size_t cap, int *is_fragment, char *err, size_t errcap);
#endif
