/* guest.h -- the feature files' helpers for guest memory and guest calls,
 * built on cpu.h's accessors.
 *
 * Not part of cpu.h: the lifted shards do not use these, and cpu.h is in
 * every shard's hash (web/build.sh HDRSUM), so a change there compiles all
 * 128 of them again. */
#ifndef NFSU2_GUEST_H
#define NFSU2_GUEST_H
#include "cpu.h"

void dispatch_indirect(CPU *c, uint32_t addr);

/* ---- addresses ----------------------------------------------------------- */
/* A nonzero address whose byte and the byte n past it are in committed pages
 * (16 KB each): a .. a + n, for an n below a page.  A field at +n of 4 bytes
 * is guest_ok(a, n + 3). */
static inline int guest_ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
/* ... the n bytes a .. a + n - 1 (n of 0: a and the byte before it) */
static inline int guest_ok_bytes(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
/* a guest dword, 0 where it is not there */
static inline uint32_t guest_rd32(uint32_t a){ return guest_ok(a, 3) ? MR32(a) : 0u; }

/* ---- strings ------------------------------------------------------------- */
/* the guest's C string at a into o (n bytes with its 0), as far as its pages
 * are committed; "" for a of 0 */
static inline void guest_read_str(uint32_t a, char *o, int n){
    int i = 0;
    for (; a && i + 1 < n && guest_ok(a + (uint32_t)i, 0) && MR8(a + (uint32_t)i); i++) o[i] = (char)MR8(a + (uint32_t)i);
    o[i] = 0;
}
/* s into guest memory at a, cap bytes at most with its 0 */
static inline void guest_write_str(uint32_t a, const char *s, uint32_t cap){
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++) MR8(a + i) = (uint8_t)s[i];
    MR8(a + i) = 0;
}

/* ---- guest calls --------------------------------------------------------- */
/* The return address of guest calls made from the main loop's per-frame hook
 * (roam.c, the hook at this address): some of the game's functions look at
 * their caller's. */
#define GUEST_RET_MAINLOOP 0x00581475u

/* The guest function fn, from a hook or an override: n stack arguments, ecx
 * and edx, then ret pushed as its return address (its ret pops it), on a
 * stack gap bytes below the current one (16-aligned).  The whole CPU is put
 * back after, so any calling convention works; eax is returned. */
static inline uint32_t guest_call_ex(CPU *c, uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *arg,
                                     uint32_t ret, uint32_t gap){
    CPU save = *c;
    c->edx = edx;                   /* first: a store of edx as it was, which the compiler then drops */
    c->esp = (c->esp - gap) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(ret);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
/* fn with ecx and stack arguments, returning to fn itself */
static inline uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    return guest_call_ex(c, fn, ecx, c->edx, n, arg, fn, 256u);
}
/* ... returning to the main-loop hook (from it only) */
static inline uint32_t gcall_main(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    return guest_call_ex(c, fn, ecx, c->edx, n, arg, GUEST_RET_MAINLOOP, 256u);
}
#endif
