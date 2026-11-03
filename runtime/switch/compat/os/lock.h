/* Switch: os_unfair_lock as a spin-then-yield lock on C11 atomics (the wasm build's, without wasm's wait/notify) */
#pragma once
#include <stdatomic.h>
#include <sched.h>
typedef struct { _Atomic int v; } os_unfair_lock;
#define OS_UNFAIR_LOCK_INIT { 0 }
static inline void os_unfair_lock_lock(os_unfair_lock *l){
    for (int spins = 0;; spins++){
        int z = 0;
        if (atomic_compare_exchange_weak_explicit(&l->v, &z, 1, memory_order_acquire, memory_order_relaxed)) return;
        if (spins > 64) sched_yield();
    }
}
static inline int os_unfair_lock_trylock(os_unfair_lock *l){
    int z = 0; return atomic_compare_exchange_strong_explicit(&l->v, &z, 1, memory_order_acquire, memory_order_relaxed);
}
static inline void os_unfair_lock_unlock(os_unfair_lock *l){ atomic_store_explicit(&l->v, 0, memory_order_release); }
static inline void os_unfair_lock_assert_owner(const os_unfair_lock *l){ (void)l; }
