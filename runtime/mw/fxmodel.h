/* fxmodel.h -- what D3DX 9 builds from a parsed fx_2_0 effect (fx2.h):
 * parameters, members, elements, annotations, techniques and passes as
 * nodes with stable ids, D3DX's lookups, value storage with its Set/Get
 * conversions and change counters, the effect pool's shared parameters,
 * and the CTAB match of every shader and preshader.  docs/mw/fx-model.md
 * is the contract and the section numbers are its.  No globals, no I/O.
 *
 * Every id, index, count and pointer that comes in is checked: a call the
 * contract does not accept returns FXM_INVALIDCALL (or 0 for a lookup) and
 * changes nothing.  An effect that fx2_parse accepted always gets a model;
 * what cannot be matched is recorded with its reason (§3.2) and nothing is
 * dropped.  The one refusal besides memory running out is a bound on the
 * model's own memory, 1024 bytes per byte of the effect plus 16 MB, which
 * no effect reaches unless it repeats huge names or arrays to blow up
 * paths or CTAB leaves (a bound on time and memory, as fx2.c's node budget
 * is); it is reported as out of memory. */
#ifndef MW_FXMODEL_H
#define MW_FXMODEL_H
#include "fx2.h"

#define FXM_OK           0x00000000u
#define FXM_INVALIDCALL  0x8876086Cu   /* D3DERR_INVALIDCALL */
#define FXM_FAIL         0x80004005u   /* E_FAIL */
#define FXM_NOTIMPL      0x80004001u
#define FXM_OUTOFMEMORY  0x8007000Eu
#define FXM_SHARED       1u            /* D3DX_PARAMETER_SHARED */
#define FXM_ANNOTATION   4u            /* D3DX_PARAMETER_ANNOTATION */

enum { FXM_K_PARAM = 1, FXM_K_ELEMENT, FXM_K_MEMBER, FXM_K_ANNOT, FXM_K_TECH, FXM_K_PASS };

typedef struct fxm_pool fxm_pool;
typedef struct fxm_effect fxm_effect;

typedef struct {                 /* a node (§2.2) */
    uint32_t kind, parent, index, top;
    fx2_str name, sem, path;     /* path: never p NULL for a parameter, element or member (an unnamed one is "") */
    uint32_t cls, type, rows, cols, elements, members;
    uint32_t bytes, flags, nannots;
    uint32_t object;             /* object element: the file's object id */
    const fx2_type *t;           /* its typedef (an element: its array's); NULL for a technique or pass */
    const fx2_block *block;      /* sampler leaf: its sampler_state block */
} fxm_node;

fxm_pool *fxm_pool_new(void);                  /* NULL: out of memory */
void      fxm_pool_free(fxm_pool *p);          /* no effect may use it any more (if one does, nothing happens) */
uint32_t  fxm_pool_slots(const fxm_pool *p);   /* slot numbers handed out so far */

/* 0, or -1 with a message (out of memory only). fx must outlive the effect. */
int  fxm_effect_new(fxm_effect **out, const fx2_effect *fx, fxm_pool *pool, char *err, uint32_t errlen);
/* drop (may be NULL) gets every non-zero object pointer whose storage dies with the effect (§2.7) */
void fxm_effect_free(fxm_effect *e, void (*drop)(void *ctx, uint32_t ptr), void *ctx);

uint32_t fxm_nodes(const fxm_effect *e);                         /* N: ids 1..N */
const fxm_node *fxm_node_get(const fxm_effect *e, uint32_t id);  /* NULL for 0 or > N */
uint32_t fxm_shared_slot(const fxm_effect *e, uint32_t id);      /* top-level id: slot + 1, 0 = not shared */

/* §2.3 */
uint32_t fxm_param(const fxm_effect *e, uint32_t parent, uint32_t index);
uint32_t fxm_element(const fxm_effect *e, uint32_t parent, uint32_t index);
uint32_t fxm_by_name(const fxm_effect *e, uint32_t parent, const uint8_t *s, uint32_t n);
uint32_t fxm_by_semantic(const fxm_effect *e, uint32_t parent, const uint8_t *s, uint32_t n);  /* s NULL: none */
uint32_t fxm_annot(const fxm_effect *e, uint32_t owner, uint32_t index);
uint32_t fxm_annot_by_name(const fxm_effect *e, uint32_t owner, const uint8_t *s, uint32_t n);
uint32_t fxm_tech(const fxm_effect *e, uint32_t index);
uint32_t fxm_tech_by_name(const fxm_effect *e, const uint8_t *s, uint32_t n);
uint32_t fxm_pass(const fxm_effect *e, uint32_t tech, uint32_t index);
uint32_t fxm_pass_by_name(const fxm_effect *e, uint32_t tech, const uint8_t *s, uint32_t n);

/* §2.5: the caller's x87 precision (24, 53 or 64; 24, MW's game thread, until set), which SetVector's colour
 * form rounds at */
void fxm_set_x87_pc(fxm_effect *e, int pc);
/* §2.5: FXM_OK or an error; changed (may be NULL) is 1 when a stored byte differs */
uint32_t fxm_set_value(fxm_effect *e, uint32_t id, const uint32_t *src, uint32_t bytes, int *changed);
uint32_t fxm_get_value(const fxm_effect *e, uint32_t id, uint32_t *dst, uint32_t bytes);
uint32_t fxm_set_bool(fxm_effect *e, uint32_t id, uint32_t b, int *changed);
uint32_t fxm_get_bool(const fxm_effect *e, uint32_t id, uint32_t *b);
uint32_t fxm_set_int(fxm_effect *e, uint32_t id, uint32_t n, int *changed);
uint32_t fxm_get_int(const fxm_effect *e, uint32_t id, uint32_t *n);
uint32_t fxm_set_float(fxm_effect *e, uint32_t id, uint32_t f, int *changed);
uint32_t fxm_get_float(const fxm_effect *e, uint32_t id, uint32_t *f);
/* type FX2_T_BOOL, FX2_T_INT or FX2_T_FLOAT: Set/Get{Bool,Int,Float}Array */
uint32_t fxm_set_array(fxm_effect *e, uint32_t id, uint32_t type, const uint32_t *src, uint32_t count, int *changed);
uint32_t fxm_get_array(const fxm_effect *e, uint32_t id, uint32_t type, uint32_t *dst, uint32_t count);
uint32_t fxm_set_vector(fxm_effect *e, uint32_t id, const uint32_t v[4], int *changed);
uint32_t fxm_get_vector(const fxm_effect *e, uint32_t id, uint32_t v[4]);
uint32_t fxm_set_vector_array(fxm_effect *e, uint32_t id, const uint32_t *v, uint32_t count, int *changed);
uint32_t fxm_get_vector_array(const fxm_effect *e, uint32_t id, uint32_t *v, uint32_t count);
uint32_t fxm_set_matrix(fxm_effect *e, uint32_t id, const uint32_t m[16], int transpose, int *changed);
uint32_t fxm_get_matrix(const fxm_effect *e, uint32_t id, uint32_t m[16], int transpose);
uint32_t fxm_set_matrix_array(fxm_effect *e, uint32_t id, const uint32_t *m, uint32_t count, int transpose,
                              int *changed);
uint32_t fxm_get_matrix_array(const fxm_effect *e, uint32_t id, uint32_t *m, uint32_t count, int transpose);
/* SetTexture, SetString: *old (may be NULL) gets the previous pointer */
uint32_t fxm_set_object(fxm_effect *e, uint32_t id, uint32_t ptr, uint32_t *old, int *changed);
uint32_t fxm_get_object(const fxm_effect *e, uint32_t id, uint32_t *ptr);

const uint32_t *fxm_data(const fxm_effect *e, uint32_t id);      /* §2.4; NULL when it has no storage */
uint64_t fxm_version(const fxm_effect *e, uint32_t id);          /* §2.6; 0 for a node outside a block */
uint64_t fxm_clock(const fxm_effect *e);

/* §3 */
enum { FXM_L_SCALAR, FXM_L_VECTOR, FXM_L_ROWS, FXM_L_COLUMNS, FXM_L_SAMPLER, FXM_L_STRUCT };
typedef struct {                 /* one register range of a matched constant (§3.3) */
    uint32_t id;                 /* the leaf node */
    uint32_t reg, count;         /* first register and count (0: nothing uploaded) */
    uint32_t layout;             /* FXM_L_SCALAR .. FXM_L_COLUMNS, FXM_L_SAMPLER */
} fxm_leaf;
typedef struct {                 /* one CTAB constant (§3.2) */
    uint32_t k;
    fx2_const c;                 /* the entry, as fx2_ctab_const reads it */
    fx2_str name;                /* p NULL: offset 0 (c.name 0) or outside the table */
    uint32_t why;                /* 0, or the reason of §3.2 */
    uint32_t id;                 /* the matched top-level parameter */
    uint32_t from, to;           /* FX2_T_BOOL/INT/FLOAT: the parameter's type, the register set's */
    uint32_t layout, per;        /* §3.3 */
    uint32_t nelem, used;        /* elements (1 for a non-array), those with registers */
    uint32_t stage;              /* sampler: the first sampler stage */
    uint32_t nleaves;
    const fxm_leaf *leaves;      /* every leaf in register order, used or not */
} fxm_const;
enum { FXM_P_SHADER, FXM_P_PRESHADER };
typedef struct {                 /* a shader or preshader resource with its matched CTAB */
    uint32_t kind;               /* FXM_P_* */
    const fx2_res *res;
    uint32_t version;            /* its version token */
    int ctab;                    /* a CTAB was found with its 28-byte header, Size 28 (§3.1) */
    uint32_t nconsts;            /* the CTAB's count, as stored */
    uint32_t nread;              /* entries read (fewer when one lies outside: reason 6) */
    const fxm_const *consts;
} fxm_prog;
enum { FXM_S_LITERAL, FXM_S_PRESHADER, FXM_S_SHADER, FXM_S_PARAM, FXM_S_UNKNOWN };
typedef struct {                 /* where a state's value comes from (§3.5) */
    uint32_t src;                /* FXM_S_* */
    uint32_t id;                 /* FXM_S_PARAM: the named parameter (0: not found) */
    const fxm_prog *prog;        /* FXM_S_PRESHADER, FXM_S_SHADER */
    const fx2_res *res;          /* the resource that decides: the last one (W); NULL for a literal */
    uint32_t nres;               /* resources on the state (1 in MW) */
} fxm_source;
fxm_source fxm_state_source(const fxm_effect *e, const fx2_state *s);
uint32_t fxm_pass_deps(const fxm_effect *e, uint32_t pass, const uint32_t **ids);   /* sorted, unique */
void     fxm_tech_versions(const fxm_effect *e, uint32_t tech, uint32_t *vs, uint32_t *ps);   /* 0: none */
/* every program in resource order (beyond §2.9: for tests and tools) */
const fxm_prog *fxm_progs(const fxm_effect *e, uint32_t *n);
#endif
