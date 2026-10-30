/* d3dxfx_mw.h -- Most Wanted's D3DX effects, native (runtime/mw/d3dxfx_mw.c): D3DXCreateEffectPool,
 * D3DXCreateEffectFromResourceA and the ID3DXEffect and ID3DXEffectPool they make (com.c's objects,
 * game.h d3dx_call), built on the effect model (fxmodel.h) and the preshader evaluator (fxpres.h). */
#ifndef D3DXFX_MW_H
#define D3DXFX_MW_H
#include <stdint.h>

struct CPU;
/* Binds the two effect imports to the HLE (each IAT slot to its thunk, the thunk's dispatch entry to the
 * answer), after win32_init and dispatch_init, before module_load_imports.  In an oracle build the oracle
 * keeps them unless NFSMW_D3DX_FX=hle; in a build without it they are always the HLE's.  Returns how many
 * it bound (0 or 2). */
int d3dxfx_install(int oracle_linked);
uint32_t d3dxfx_call(struct CPU *c, int pool, unsigned method, const uint32_t *args, int nargs);
void d3dxfx_free(struct CPU *c, int pool, uint32_t obj);

/* NFSU2_DXREC's context line for a call of the game's into an effect (the HLE's or the oracle's,
 * fxcensus_mw.c): "E<id> <method>[ <arg>]", id the effect's row in the game's list (-1: none) */
void fxmark(int id, const char *method, const char *arg, uint32_t hr);
const char *fxmark_name(int slot);              /* ID3DXEffect's slot names (d3d9_tables.h M_D3DXFX) */
int fx_wrapper_id(uint32_t effect);             /* the game's effect list row holding it, -1: none */
#endif
