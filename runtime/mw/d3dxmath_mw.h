/* d3dxmath_mw.h -- Most Wanted's native D3DX math (d3dxmath_mw.c): the eleven
 * math functions speed.exe imports from d3dx9_26.dll (GAME.md 3.3). */
#ifndef D3DXMATH_MW_H
#define D3DXMATH_MW_H
#include <stdint.h>

struct CPU;
#define D3DXMATH_COUNT 11
typedef struct D3dxMathFn {
    const char *name;               /* the export's name */
    uint32_t iat;                   /* speed.exe's IAT slot of it */
    uint32_t thunk;                 /* speed.exe's `jmp [iat]` for it */
    int nargs;                      /* stdcall: the answer pops them */
    void (*fn)(struct CPU *c);      /* the answer, called as the thunk's target (return address at esp) */
} D3dxMathFn;
extern const D3dxMathFn d3dxmath_fns[D3DXMATH_COUNT];

/* Binds the eleven IAT slots to the answers (each slot to its thunk, the thunk's dispatch entry to the
 * answer); after win32_init and dispatch_init, before module_load_imports.  With NFSMW_D3DX_MATH=oracle
 * and an oracle linked (oracle_linked) it binds none.  Returns how many it bound. */
int d3dxmath_install(int oracle_linked);
#endif
