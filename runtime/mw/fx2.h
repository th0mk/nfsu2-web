/* fx2.h -- compiled D3DX effects (fx_2_0, tag 0xFEFF0901), as Most Wanted
 * carries them in speed.exe's resources: the container, the D3D9 shader
 * token walk, constant tables (CTAB) and FXLC preshaders.  The format is
 * docs/mw/fx2-format.md; the section numbers below are its.
 *
 * fx2_parse reads an effect into a tree that points into the caller's
 * buffer: names, values and blobs are not copied, so the buffer must outlive
 * the tree.  Every offset, count and size is checked against the buffer, so a
 * malformed effect gives an error message, never a read outside it.  A field
 * value past its table (a type, class, state operation, usage or object id,
 * a resource whose target does not exist) is kept as it is for the caller to
 * judge: only structure that cannot be read is an error.
 *
 * The blob readers (fx2_shader_walk, fx2_ctab_*, fx2_pres_read, fx2_clit,
 * fx2_fxlc_*) take any byte range and never read outside it either.  Every
 * field is read byte by byte, little-endian, so nothing needs alignment.
 */
#ifndef MW_FX2_H
#define MW_FX2_H
#include <stdint.h>

#define FX2_TAG          0xFEFF0901u   /* §2.1: effect binary 9.1 */
#define FX2_END          0x0000FFFFu   /* the last token of a shader or preshader */
#define FX2_PRES_VERSION 0x46580200u   /* 'FX' 2.0: a preshader's version token */
#define FX2_CTAB         0x42415443u   /* comment FOURCCs (§4.2, §4.3) */
#define FX2_CLIT         0x54494C43u
#define FX2_FXLC         0x434C5846u
#define FX2_NONE         0xFFFFFFFFu   /* resource technique or element: none */
#define FX2_NSTATES      182           /* rows of d3dx9_26's state table (§3.1) */
#define FX2_DEPTH        32            /* nesting of typedefs and values a parse allows (§5.11) */

/* D3DXPARAMETER_TYPE and D3DXPARAMETER_CLASS (§2.3) */
enum { FX2_T_VOID, FX2_T_BOOL, FX2_T_INT, FX2_T_FLOAT, FX2_T_STRING, FX2_T_TEXTURE, FX2_T_TEXTURE1D,
       FX2_T_TEXTURE2D, FX2_T_TEXTURE3D, FX2_T_TEXTURECUBE, FX2_T_SAMPLER, FX2_T_SAMPLER1D,
       FX2_T_SAMPLER2D, FX2_T_SAMPLER3D, FX2_T_SAMPLERCUBE, FX2_T_PIXELSHADER, FX2_T_VERTEXSHADER,
       FX2_T_PIXELFRAGMENT, FX2_T_VERTEXFRAGMENT, FX2_T_UNSUPPORTED, FX2_NTYPES };
enum { FX2_C_SCALAR, FX2_C_VECTOR, FX2_C_MATRIX_ROWS, FX2_C_MATRIX_COLUMNS, FX2_C_OBJECT, FX2_C_STRUCT,
       FX2_NCLASSES };

/* how a value's data is laid out (§2.4), from its typedef's class and type */
enum { FX2_V_NUMERIC,      /* rows x columns dwords per element */
       FX2_V_OBJECT,       /* one object id per element */
       FX2_V_SAMPLER,      /* a sampler_state block per element */
       FX2_V_STRUCT,       /* the members' values per element */
       FX2_V_UNKNOWN };    /* a class past the table: nothing is read */

typedef struct { const uint8_t *p; uint32_t n; } fx2_str;   /* up to the first NUL; p NULL: offset 0 */

typedef struct fx2_type fx2_type;
struct fx2_type {                /* a typedef (§2.3) */
    uint32_t off;                /* body offset */
    uint32_t type, cls;
    fx2_str name, sem;
    uint32_t elements;           /* 0: not an array */
    uint32_t rows, cols;         /* numeric classes, rows x columns (a vector is 1 x its columns) */
    uint32_t nmembers;           /* STRUCT */
    fx2_type *members;
};

typedef struct fx2_state fx2_state;
typedef struct fx2_res fx2_res;
typedef struct fx2_value fx2_value;

typedef struct {                 /* a sampler_state block: one element of a sampler value */
    uint32_t nstates;
    fx2_state *states;
} fx2_block;

struct fx2_value {               /* a value with its typedef (§2.4) */
    const fx2_type *t;
    uint32_t kind;               /* FX2_V_* */
    uint32_t n;                  /* elements read: the element count, or 1 when it is 0 */
    uint32_t off, size;          /* body offset, and the bytes it takes from there */
    const uint8_t *data;         /* NUMERIC: n x rows x cols dwords; OBJECT: n object ids */
    fx2_block *blocks;           /* SAMPLER: n blocks */
    fx2_value *members;          /* STRUCT: n x nmembers values, element after element */
};

struct fx2_state {               /* a state assignment (§2.5.5) */
    uint32_t op, index;          /* the state table row (fx2_state_name), its index (§3.3) */
    fx2_type *t;
    fx2_value v;
    fx2_res *res;                /* the first resource that targets it (then res->next), or NULL */
};

typedef struct {                 /* an annotation (§2.5.2); its typedef's name is its name */
    fx2_type *t;
    fx2_value v;
} fx2_annot;

typedef struct {                 /* a parameter (§2.5.1) */
    fx2_type *t;
    fx2_value v;                 /* the default value */
    uint32_t flags;              /* bit 0: shared through the effect pool */
    uint32_t nannots;
    fx2_annot *annots;
} fx2_param;

typedef struct {                 /* a pass (§2.5.4) */
    fx2_str name;
    uint32_t nannots, nstates;
    fx2_annot *annots;
    fx2_state *states;
} fx2_pass;

typedef struct {                 /* a technique (§2.5.3) */
    fx2_str name;
    uint32_t nannots, npasses;
    fx2_annot *annots;
    fx2_pass *passes;
} fx2_tech;

typedef struct {                 /* an object-data ("string") entry (§2.5.6) */
    uint32_t id, size;
    const uint8_t *data;
} fx2_odata;

struct fx2_res {                 /* a resource entry (§2.5.7) */
    uint32_t tech, index, element, state, usage, size;
    const uint8_t *data;
    fx2_state *target;           /* the state it belongs to (§5.6), NULL when there is none; on a sampler
                                  * that is no array the element is ignored, as D3DX does (§2.5.7) */
    fx2_res *next;               /* the next resource on the same state, in file order */
};

typedef struct {                 /* an object id (§2.7) */
    int held;                    /* an element of an object value holds it */
    uint32_t type;               /* held: the type of the first such value, in file order */
} fx2_object;

typedef struct {
    const uint8_t *file;
    uint32_t size;
    uint32_t tag, tables, data0;           /* the two header dwords, the dword at body +0 */
    uint32_t nparams, ntechs, shaders, nobjects, nodata, nres;   /* §2.5's counts, as stored */
    fx2_param *params;
    fx2_tech *techs;
    fx2_object *objects;
    fx2_odata *odata;
    fx2_res *res;
    void *mem;                   /* the tree's allocations, for fx2_free */
} fx2_effect;

/* Parse size bytes at file.  0, or -1 with a message in err (and nothing to
 * free).  The tag is not checked: fx->tag is for the caller. */
int  fx2_parse(fx2_effect *fx, const uint8_t *file, uint32_t size, char *err, uint32_t errlen);
void fx2_free(fx2_effect *fx);

const char *fx2_type_name(uint32_t type);       /* the names of §2.3; NULL past the table */
const char *fx2_class_name(uint32_t cls);
const char *fx2_state_name(uint32_t op);        /* the names of §3.1 */
uint32_t    fx2_value_kind(const fx2_type *t);  /* FX2_V_* */

static inline uint32_t fx2_rd32(const uint8_t *p){
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint32_t fx2_rd16(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* ---- shaders and preshaders: size bytes at p, read as size / 4 dwords ---- */

/* a comment token at dword pos with L = len: called in walk order */
typedef void (*fx2_comment_fn)(void *ctx, uint32_t pos, uint32_t len);

/* The bytes of the comment at dword pos (length len) that lie inside a blob
 * of ndw dwords, from the dword after its token (the FOURCC first). */
uint32_t fx2_comment_bytes(uint32_t ndw, uint32_t pos, uint32_t len);

typedef struct {                 /* a D3D9 shader's token walk (§4.1, §5.9) */
    uint32_t version, ndw;       /* dword 0 (0 for a blob under 4 bytes), size / 4 */
    int known;                   /* version high word 0xFFFE (vs) or 0xFFFF (ps): walked */
    uint32_t ninstr, ncomments;  /* instruction tokens (comments not counted), comments */
    uint32_t stop;               /* the dword index where the walk stopped (ndw or more: ran out) */
    int ended;                   /* it stopped on the end token as the last dword */
} fx2_shader;
void fx2_shader_walk(const uint8_t *p, uint32_t size, fx2_shader *s, fx2_comment_fn fn, void *ctx);

typedef struct {                 /* a constant table (§4.2): the bytes after the CTAB FOURCC */
    const uint8_t *p;
    uint32_t n;
    uint32_t size, creator, version, nconsts, cinfo, flags, target;
} fx2_ctab;

typedef struct {                 /* D3DXSHADER_CONSTANTINFO with its D3DXSHADER_TYPEINFO */
    uint32_t name, set, reg, count, reserved, typeinfo, defval;
    int typed;                   /* the TYPEINFO lies inside the table: the fields below are read */
    uint32_t cls, type, rows, cols, elements, members, memberinfo;
} fx2_const;

int fx2_ctab_init(fx2_ctab *c, const uint8_t *p, uint32_t n);    /* 0, or -1 under 28 bytes */
int fx2_ctab_const(const fx2_ctab *c, uint32_t k, fx2_const *o); /* 0, or -1: entry k is outside */
int fx2_ctab_str(const fx2_ctab *c, uint32_t off, fx2_str *s);   /* 1, 0 for offset 0, -1 outside */

typedef struct {                 /* an FXLC preshader's frame (§4.3, §5.10) */
    uint32_t version, ndw;
    int known;                   /* version high word 0x4658 */
    uint32_t ncomments;
    uint32_t stop;               /* the dword index after the run of comments from dword 1 */
    int ended;                   /* that dword is the end token and the last dword */
    uint32_t ctab, clit, fxlc;   /* the first comment with each FOURCC: its token's dword index, 0 = none */
    uint32_t ctab_len, clit_len, fxlc_len;   /* their L */
} fx2_pres;
void fx2_pres_read(const uint8_t *p, uint32_t size, fx2_pres *ps, fx2_comment_fn fn, void *ctx);

/* The CLIT table in the n bytes q after its FOURCC: the count and the
 * doubles (8 bytes each).  0, -1 when there is no count, -2 when the count
 * does not fit. */
int fx2_clit(const uint8_t *q, uint32_t n, uint32_t *count, const uint8_t **lits);

/* FXLC: in the ndw dwords q after its FOURCC, dword 0 is the instruction
 * count and the instructions follow from dword 1 (§4.3). */
typedef struct { uint32_t flag, table, offset, itable, ioffset; } fx2_operand;
enum { FX2_INS_OK,               /* the whole instruction is inside */
       FX2_INS_FLAG,             /* stopped on an operand flag other than 0 or 1 */
       FX2_INS_SHORT };          /* it would read past the end */
typedef struct {
    int status;                  /* FX2_INS_* */
    uint32_t pos;                /* the token's dword index */
    uint32_t token, opcode, ncomp, scalar, nin;   /* bits 20..30, 0..15, 31; the input count */
    uint32_t in;                 /* the first input operand's dword index */
    uint32_t end;                /* OK: the dword after the instruction; FLAG: the flag's dword */
    uint32_t flag;               /* FLAG: the flag */
    fx2_operand out;             /* OK: the output operand */
} fx2_ins;
int fx2_fxlc_ins(const uint8_t *q, uint32_t ndw, uint32_t pos, fx2_ins *ins);   /* ins->status */
/* the operand at dword pos: the dword after it, or 0 when it is not whole inside ndw */
uint32_t fx2_fxlc_operand(const uint8_t *q, uint32_t ndw, uint32_t pos, fx2_operand *o);
const char *fx2_fxlc_name(uint32_t opcode, uint32_t nin);   /* §4.3's opcodes; NULL when not there */
const char *fx2_fxlc_table(uint32_t code);                   /* imm c v oc ob oi r; NULL past them */

#endif
