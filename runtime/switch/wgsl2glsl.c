/* wgsl2glsl.c -- see wgsl2glsl.h.  Tokenizer, a recursive-descent parser that prints GLSL as it goes, and a small type
 * inference (a `let e = ...` becomes `vec4 e = ...`).  Self-contained C (no runtime headers): tested natively against every
 * module the game registers (runtime/switch/tests/shader_to_glsl.c). */
#include "wgsl2glsl.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static char g_tb_name[40];            /* the storage array read through a buffer texture (see tbuf_on) */
#include <string.h>

/* ---- types: base (F float, U uint, I int, B bool), n (1 scalar, 2..4 vector), 'm' matrix 4x4, 'S' struct, 'A' array, '?' ---- */
typedef struct Ty { char base; char n; short sid; const struct Ty *elem; } Ty;
static const Ty T_F = { 'F', 1, 0, 0 }, T_U = { 'U', 1, 0, 0 }, T_I = { 'I', 1, 0, 0 }, T_B = { 'B', 1, 0, 0 }, T_UNK = { '?', 0, 0, 0 };
static Ty vecty(char base, int n){ Ty t = { base, (char)n, 0, 0 }; return t; }

/* ---- output buffer ---- */
typedef struct { char *p; size_t cap, n; int ok; } Out;
static void put(Out *o, const char *fmt, ...){
    if (!o->ok) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(o->p + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= o->cap - o->n){ o->ok = 0; return; }
    o->n += (size_t)w;
}

/* ---- tokens ---- */
typedef enum { K_EOF, K_ID, K_NUM, K_PUNCT } Kind;
typedef struct { Kind k; char s[40]; } Tok;
typedef struct { Tok *t; int n, i; char *err; size_t errcap; int bad; } Lex;

static int lex(const char *s, Lex *L){
    int cap = 4096; L->t = malloc(sizeof(Tok) * (size_t)cap); L->n = 0; L->i = 0;
    if (!L->t) return -1;
    while (*s){
        if (isspace((unsigned char)*s)){ s++; continue; }
        if (s[0] == '/' && s[1] == '/'){ while (*s && *s != '\n') s++; continue; }
        if (L->n + 2 >= cap){ cap *= 2; Tok *nt = realloc(L->t, sizeof(Tok) * (size_t)cap); if (!nt) return -1; L->t = nt; }
        Tok *t = &L->t[L->n++];
        if (isalpha((unsigned char)*s) || *s == '_'){
            int k = 0; while ((isalnum((unsigned char)*s) || *s == '_') && k < 38) t->s[k++] = *s++;
            t->s[k] = 0; t->k = K_ID; continue;
        }
        if (isdigit((unsigned char)*s) || (*s == '.' && isdigit((unsigned char)s[1]))){
            int k = 0;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')){                 /* hex int, or hex float 0x1.8p-3 */
                while ((isxdigit((unsigned char)*s) || *s == 'x' || *s == 'X' || *s == '.' || *s == 'p' || *s == 'P' ||
                        ((*s == '-' || *s == '+') && (s[-1] == 'p' || s[-1] == 'P'))) && k < 38) t->s[k++] = *s++;
            } else {
                while ((isdigit((unsigned char)*s) || *s == '.' || *s == 'e' || *s == 'E' ||
                        ((*s == '-' || *s == '+') && (s[-1] == 'e' || s[-1] == 'E'))) && k < 38) t->s[k++] = *s++;
            }
            while ((*s == 'u' || *s == 'i' || *s == 'f' || *s == 'h') && k < 38) t->s[k++] = *s++;
            t->s[k] = 0; t->k = K_NUM; continue;
        }
        static const char *two[] = { "->", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "++", "--", "+=", "-=", "*=", "/=", "|=", "&=", "^=", "%=" };
        int found = 0;
        for (unsigned q = 0; q < sizeof two / sizeof *two; q++) if (s[0] == two[q][0] && s[1] == two[q][1]){ t->s[0] = s[0]; t->s[1] = s[1]; t->s[2] = 0; s += 2; found = 1; break; }
        if (!found){ t->s[0] = *s++; t->s[1] = 0; }
        t->k = K_PUNCT;
    }
    L->t[L->n].k = K_EOF; L->t[L->n].s[0] = 0; L->n++;
    return 0;
}
static const Tok *peek(Lex *L, int ahead){ int i = L->i + ahead; return &L->t[i < L->n ? i : L->n - 1]; }
static int is(Lex *L, const char *s){ const Tok *t = peek(L, 0); return t->k != K_EOF && !strcmp(t->s, s); }
static int is_at(Lex *L, int a, const char *s){ const Tok *t = peek(L, a); return t->k != K_EOF && !strcmp(t->s, s); }
static void fail(Lex *L, const char *what){
    if (!L->bad){ L->bad = 1; if (L->err) snprintf(L->err, L->errcap, "%s at token %d '%s'", what, L->i, peek(L, 0)->s); }
}
static int eat(Lex *L, const char *s){ if (is(L, s)){ L->i++; return 1; } return 0; }
static void expect(Lex *L, const char *s){ if (!eat(L, s)){ char m[80]; snprintf(m, sizeof m, "expected '%s'", s); fail(L, m); } }
static const char *ident(Lex *L){ const Tok *t = peek(L, 0); if (t->k != K_ID){ fail(L, "expected identifier"); return ""; } L->i++; return t->s; }

/* ---- symbols ---- */
typedef struct { char name[40]; Ty ty; int depth; } Sym;
typedef struct { char name[40]; Ty ret; } Fn;
typedef struct { char name[40]; char mem[16][40]; Ty mty[16]; int nm; } StructDef;
typedef struct {
    Lex *L; Out *o;
    Sym syms[512]; int nsym, depth;
    Fn fns[64]; int nfn;
    StructDef st[16]; int nst;
    char entry[40]; int is_frag; int entry_vertex_index, entry_instance_index; char entry_params[4][40]; int nentry_params; char entry_pbuiltin[4][24];
    int uses_vout_param; char vout_param[40];
} P;

static int struct_id(P *p, const char *name){ for (int i = 0; i < p->nst; i++) if (!strcmp(p->st[i].name, name)) return i; return -1; }
static void sym_add(P *p, const char *name, Ty ty){ if (p->nsym < 512){ snprintf(p->syms[p->nsym].name, 40, "%s", name); p->syms[p->nsym].ty = ty; p->syms[p->nsym].depth = p->depth; p->nsym++; } }
static const Sym *sym_find(P *p, const char *name){ for (int i = p->nsym - 1; i >= 0; i--) if (!strcmp(p->syms[i].name, name)) return &p->syms[i]; return NULL; }
static const Fn *fn_find(P *p, const char *name){ for (int i = 0; i < p->nfn; i++) if (!strcmp(p->fns[i].name, name)) return &p->fns[i]; return NULL; }
static void scope_push(P *p){ p->depth++; }
static void scope_pop(P *p){ while (p->nsym && p->syms[p->nsym - 1].depth >= p->depth) p->nsym--; p->depth--; }

/* identifiers GLSL reserves or defines that a WGSL module may use as a name */
static const char *glsl_name(const char *n, char *buf){
    static const char *bad[] = { "input", "output", "sample", "texture", "filter", "common", "partition", "active", "smooth", "flat", "half", "fixed",
        "packed", "precise", "buffer", "shared", "coherent", "patch", "subroutine", "in", "out", "inout", "uniform", "attribute", "varying", "layout",
        "centroid", "noperspective", "invariant", "resource", "volatile", "restrict", "readonly", "writeonly", "atomic_uint", "namespace", "using",
        "template", "this", "goto", "union", "enum", "typedef", "sizeof", "cast", "short", "long", "double", "float", "int", "uint", "bool", "void", "lowp",
        "mediump", "highp", "mat2", "mat3", "mat4", "vec2", "vec3", "vec4", "main", "mod", "step", "sign", "floor", "ceil", "fract", "clamp", "mix", "min",
        "max", "abs", "pow", "exp", "log", "exp2", "log2", "sqrt", "dot", "cross", "length", "normalize", "reflect", "refract", "radians", "degrees", "sin", "cos", "tan" };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) if (!strcmp(n, bad[i])){ snprintf(buf, 48, "%s_", n); return buf; }
    return n;
}

/* ---- type syntax ---- */
static Ty parse_type(P *p);
static const char *glsl_type_name(Ty t, char *buf){
    if (t.base == 'F') snprintf(buf, 24, t.n == 1 ? "float" : "vec%d", t.n);
    else if (t.base == 'U') snprintf(buf, 24, t.n == 1 ? "uint" : "uvec%d", t.n);
    else if (t.base == 'I') snprintf(buf, 24, t.n == 1 ? "int" : "ivec%d", t.n);
    else if (t.base == 'B') snprintf(buf, 24, t.n == 1 ? "bool" : "bvec%d", t.n);
    else if (t.base == 'm') snprintf(buf, 24, "mat4");
    else snprintf(buf, 24, "int");
    return buf;
}
static Ty parse_type(P *p){
    Lex *L = p->L;
    const char *n = ident(L);
    if (!strcmp(n, "f32")) return T_F;
    if (!strcmp(n, "u32")) return T_U;
    if (!strcmp(n, "i32")) return T_I;
    if (!strcmp(n, "bool")) return T_B;
    if (n[0] == 'v' && n[1] == 'e' && n[2] == 'c' && n[3] >= '2' && n[3] <= '4'){
        int sz = n[3] - '0';
        char base = n[4] == 'u' ? 'U' : n[4] == 'i' ? 'I' : 'F';
        if (n[4] == 0 && is(L, "<")){ L->i++; Ty e = parse_type(p); expect(L, ">"); base = e.base; }
        return vecty(base, sz);
    }
    if (!strncmp(n, "mat", 3)){ Ty t = { 'm', 4, 0, 0 }; if (is(L, "<")){ L->i++; parse_type(p); expect(L, ">"); } return t; }
    int sid = struct_id(p, n);
    if (sid >= 0){ Ty t = { 'S', 0, (short)sid, 0 }; return t; }
    fail(L, "unknown type");
    return T_UNK;
}
/* array<T, N> or array<T> after the name was read: returns the element type; *count is the size (0 = runtime sized) */
static Ty parse_array_type(P *p, int *count, Ty *store){
    Lex *L = p->L; *count = 0;
    expect(L, "<"); *store = parse_type(p);
    if (eat(L, ",")){ const Tok *t = peek(L, 0); *count = atoi(t->s); L->i++; }
    expect(L, ">");
    Ty a = { 'A', 0, 0, store };
    return a;
}

/* ---- expressions ---- */
typedef struct { char *s; Ty ty; } E;           /* printed GLSL + type */
static char *dupstr(const char *s){ size_t n = strlen(s) + 1; char *r = malloc(n); if (r) memcpy(r, s, n); return r; }
static char *fmt_alloc(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    char *r = malloc((size_t)n + 1);
    if (r) vsnprintf(r, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return r;
}
static E mk(char *s, Ty t){ E e = { s, t }; return e; }
static E parse_expr(P *p);
static E parse_unary(P *p);

static Ty swizzle_ty(Ty base, int len){
    char b = base.base == '?' ? 'F' : base.base;
    return len == 1 ? (Ty){ b, 1, 0, 0 } : vecty(b, len);
}
static Ty member_ty(P *p, Ty t, const char *m, int *is_swizzle){
    *is_swizzle = 0;
    if (t.base == 'S'){
        StructDef *sd = &p->st[t.sid];
        for (int i = 0; i < sd->nm; i++) if (!strcmp(sd->mem[i], m)) return sd->mty[i];
        return T_UNK;
    }
    if (t.base == 'F' || t.base == 'U' || t.base == 'I' || t.base == 'B'){
        int ok = 1; for (const char *q = m; *q; q++) if (!strchr("xyzwrgba", *q)) ok = 0;
        if (ok){ *is_swizzle = 1; return swizzle_ty(t, (int)strlen(m)); }
    }
    return T_UNK;
}
static const char *glsl_fn(const char *n){
    if (!strcmp(n, "inverseSqrt")) return "inversesqrt";
    if (!strcmp(n, "saturate")) return "clamp";
    return n;
}
static Ty arith_ty(Ty a, Ty b){
    if (a.n > 1 && b.n <= 1) return a;
    if (b.n > 1 && a.n <= 1) return b;
    if (a.base == '?') return b;
    return a;
}

static E parse_args_call(P *p, const char *name, int has_bitcast, Ty bitcast_to){
    Lex *L = p->L;
    E args[8]; int na = 0;
    expect(L, "(");
    while (!is(L, ")") && !L->bad && na < 8){ args[na++] = parse_expr(p); if (!eat(L, ",")) break; }
    expect(L, ")");
    char buf[48]; (void)buf;
    char *s = NULL; Ty ty = T_UNK;
    /* constructors and conversions */
    if (!strcmp(name, "f32") || !strcmp(name, "u32") || !strcmp(name, "i32") || !strcmp(name, "bool")){
        Ty t = !strcmp(name, "f32") ? T_F : !strcmp(name, "u32") ? T_U : !strcmp(name, "i32") ? T_I : T_B;
        s = fmt_alloc("%s(%s)", t.base == 'F' ? "float" : t.base == 'U' ? "uint" : t.base == 'I' ? "int" : "bool", na ? args[0].s : "0"); ty = t;
    } else if (name[0] == 'v' && name[1] == 'e' && name[2] == 'c' && name[3] >= '2' && name[3] <= '4' && !name[5]){
        int sz = name[3] - '0'; char base = name[4] == 'u' ? 'U' : name[4] == 'i' ? 'I' : 'F';
        ty = vecty(base, sz);
        char tn[24]; glsl_type_name(ty, tn);
        size_t len = strlen(tn) + 4; for (int i = 0; i < na; i++) len += strlen(args[i].s) + 2;
        s = malloc(len); if (s){ s[0] = 0; strcat(s, tn); strcat(s, "("); for (int i = 0; i < na; i++){ if (i) strcat(s, ", "); strcat(s, args[i].s); } strcat(s, ")"); }
    } else if (has_bitcast){
        Ty src = na ? args[0].ty : T_UNK; ty = bitcast_to;
        const char *a = na ? args[0].s : "0";
        if (ty.base == 'F') s = fmt_alloc("%s(%s)", src.base == 'I' ? "intBitsToFloat" : "uintBitsToFloat", a);
        else if (ty.base == 'U') s = fmt_alloc(src.base == 'F' ? "floatBitsToUint(%s)" : "uint(%s)", a);
        else if (ty.base == 'I') s = fmt_alloc(src.base == 'F' ? "floatBitsToInt(%s)" : "int(%s)", a);
        else s = dupstr(a);
        if (ty.n > 1 && ty.base == 'U' && src.base != 'F'){ free(s); char tn[24]; glsl_type_name(ty, tn); s = fmt_alloc("%s(%s)", tn, a); }
        if (ty.n > 1 && ty.base == 'I' && src.base != 'F'){ free(s); char tn[24]; glsl_type_name(ty, tn); s = fmt_alloc("%s(%s)", tn, a); }
    } else if (!strcmp(name, "select") && na == 3){
        /* select(f, t, cond) -> cond ? t : f (a vector condition: mix) */
        ty = args[0].ty.base == '?' ? args[1].ty : args[0].ty;
        if (args[2].ty.n > 1) s = fmt_alloc("mix(%s, %s, %s)", args[0].s, args[1].s, args[2].s);
        else s = fmt_alloc("(%s ? %s : %s)", args[2].s, args[1].s, args[0].s);
    } else if (!strcmp(name, "saturate") && na == 1){
        ty = args[0].ty; s = fmt_alloc("clamp(%s, 0.0, 1.0)", args[0].s);
    } else if (!strcmp(name, "textureSample") && na >= 3){
        ty = vecty('F', 4); s = fmt_alloc("texture(%s, %s)", args[0].s, args[2].s);
    } else {
        const Fn *f = fn_find(p, name);
        if (f) ty = f->ret;
        else if (!strcmp(name, "dot") || !strcmp(name, "length") || !strcmp(name, "distance")) ty = T_F;
        else if (!strcmp(name, "cross")) ty = vecty('F', 3);
        else if (na) { ty = args[0].ty; if (ty.n <= 1 && na > 1 && args[1].ty.n > 1) ty = args[1].ty; }
        char tmp[48]; const char *gn = glsl_fn(name);          /* a builtin keeps its name; only the module's own functions are renamed away from GLSL words */
        if (f) gn = glsl_name(name, tmp);
        size_t len = strlen(gn) + 4; for (int i = 0; i < na; i++) len += strlen(args[i].s) + 2;
        s = malloc(len); if (s){ s[0] = 0; strcat(s, gn); strcat(s, "("); for (int i = 0; i < na; i++){ if (i) strcat(s, ", "); strcat(s, args[i].s); } strcat(s, ")"); }
    }
    for (int i = 0; i < na; i++) free(args[i].s);
    return mk(s ? s : dupstr("0"), ty);
}

static E number(P *p, const char *t){
    (void)p;
    size_t n = strlen(t);
    int is_hex = t[0] == '0' && (t[1] == 'x' || t[1] == 'X');
    int has_u = n && t[n - 1] == 'u', has_i = n && t[n - 1] == 'i', has_f = n && t[n - 1] == 'f' && !is_hex;
    char core[40]; snprintf(core, sizeof core, "%s", t);
    if (has_u || has_i || has_f) core[n - 1] = 0;
    int is_float = has_f || (!is_hex && (strchr(core, '.') || strchr(core, 'e') || strchr(core, 'E'))) || (is_hex && (strchr(core, '.') || strchr(core, 'p') || strchr(core, 'P')));
    if (is_float){
        double d = strtod(core, NULL);
        char b[48]; snprintf(b, sizeof b, "%.9g", d);
        if (!strchr(b, '.') && !strchr(b, 'e') && !strchr(b, 'n') && !strchr(b, 'i')) strcat(b, ".0");
        return mk(dupstr(b), T_F);
    }
    if (has_u) return mk(fmt_alloc("%su", core), T_U);
    return mk(dupstr(core), T_I);
}

static E parse_primary(P *p){
    Lex *L = p->L; const Tok *t = peek(L, 0);
    E e = mk(dupstr("0"), T_UNK);
    if (t->k == K_NUM){ L->i++; free(e.s); e = number(p, t->s); }
    else if (is(L, "(")){ L->i++; free(e.s); E in = parse_expr(p); expect(L, ")"); e = mk(fmt_alloc("(%s)", in.s), in.ty); free(in.s); }
    else if (t->k == K_ID){
        char name[40]; snprintf(name, sizeof name, "%s", t->s); L->i++;
        if (!strcmp(name, "true") || !strcmp(name, "false")){ free(e.s); e = mk(dupstr(name), T_B); }
        else if (!strcmp(name, "bitcast")){ expect(L, "<"); Ty to = parse_type(p); expect(L, ">"); free(e.s); e = parse_args_call(p, name, 1, to); }
        else if (is(L, "<") && (!strncmp(name, "vec", 3) || !strncmp(name, "mat", 3))){      /* vec4<f32>(...) */
            L->i++; Ty el = parse_type(p); expect(L, ">");
            char nm[8]; snprintf(nm, sizeof nm, "vec%c%c", name[3], el.base == 'U' ? 'u' : el.base == 'I' ? 'i' : 'f');
            free(e.s); e = parse_args_call(p, nm, 0, T_UNK);
        }
        else if (is(L, "(")){ free(e.s); e = parse_args_call(p, name, 0, T_UNK); }
        else {
            const Sym *s = sym_find(p, name);
            char tmp[48];
            free(e.s); e = mk(dupstr(glsl_name(name, tmp)), s ? s->ty : T_UNK);
        }
    } else { fail(L, "unexpected token in expression"); L->i++; }
    /* postfix: .member and [index] */
    for (;;){
        if (is(L, ".")){
            L->i++; const char *m = ident(L); int sw;
            Ty mt = member_ty(p, e.ty, m, &sw);
            char *ns = fmt_alloc("%s.%s", e.s, m); free(e.s); e = mk(ns, mt);
        } else if (is(L, "[")){
            L->i++; E ix = parse_expr(p); expect(L, "]");
            Ty et = T_UNK;
            if (e.ty.base == 'A' && e.ty.elem) et = *e.ty.elem;
            else if (e.ty.n > 1) et = swizzle_ty(e.ty, 1);
            char *ns = (g_tb_name[0] && e.ty.base == 'A' && !strcmp(e.s, g_tb_name)) ? fmt_alloc("texelFetch(%s_tb, int(%s)).x", e.s, ix.s) : fmt_alloc("%s[%s]", e.s, ix.s); free(e.s); free(ix.s); e = mk(ns, et);
        } else break;
    }
    return e;
}
static E parse_unary(P *p){
    Lex *L = p->L;
    if (is(L, "-") || is(L, "!") || is(L, "~")){
        char op[2] = { peek(L, 0)->s[0], 0 }; L->i++;
        E in = parse_unary(p); E r = mk(fmt_alloc("(%s%s)", op, in.s), op[0] == '!' ? T_B : in.ty); free(in.s); return r;
    }
    return parse_primary(p);
}
static int prec(const char *op){
    if (!strcmp(op, "*") || !strcmp(op, "/") || !strcmp(op, "%")) return 10;
    if (!strcmp(op, "+") || !strcmp(op, "-")) return 9;
    if (!strcmp(op, "<<") || !strcmp(op, ">>")) return 8;
    if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) return 7;
    if (!strcmp(op, "==") || !strcmp(op, "!=")) return 6;
    if (!strcmp(op, "&")) return 5;
    if (!strcmp(op, "^")) return 4;
    if (!strcmp(op, "|")) return 3;
    if (!strcmp(op, "&&")) return 2;
    if (!strcmp(op, "||")) return 1;
    return -1;
}
static E parse_bin(P *p, int minp){
    Lex *L = p->L;
    E lhs = parse_unary(p);
    for (;;){
        const Tok *t = peek(L, 0);
        if (t->k != K_PUNCT) break;
        int pr = prec(t->s);
        if (pr < minp || pr < 0) break;
        char op[4]; snprintf(op, sizeof op, "%s", t->s); L->i++;
        E rhs = parse_bin(p, pr + 1);
        Ty ty;
        if (pr == 7 || pr == 6 || pr == 2 || pr == 1) ty = (lhs.ty.n > 1 && pr >= 6 && pr <= 7) ? vecty('B', lhs.ty.n) : T_B;
        else if (pr == 8) ty = lhs.ty;
        else ty = arith_ty(lhs.ty, rhs.ty);
        char *s;
        if ((pr == 7 || pr == 6) && (lhs.ty.n > 1 || rhs.ty.n > 1)){        /* a vector comparison: GLSL spells it as a function */
            const char *fn = !strcmp(op, "<") ? "lessThan" : !strcmp(op, ">") ? "greaterThan" : !strcmp(op, "<=") ? "lessThanEqual" :
                             !strcmp(op, ">=") ? "greaterThanEqual" : !strcmp(op, "==") ? "equal" : "notEqual";
            s = fmt_alloc("%s(%s, %s)", fn, lhs.s, rhs.s);
        } else s = fmt_alloc("(%s %s %s)", lhs.s, op, rhs.s);
        free(lhs.s); free(rhs.s); lhs = mk(s, ty);
    }
    return lhs;
}
static E parse_expr(P *p){ return parse_bin(p, 1); }

/* ---- statements ---- */
static void parse_block(P *p, int indent);
static void pad(P *p, int indent){ for (int i = 0; i < indent; i++) put(p->o, "  "); }

static int is_assign_op(const char *s){ return !strcmp(s, "=") || !strcmp(s, "+=") || !strcmp(s, "-=") || !strcmp(s, "*=") || !strcmp(s, "/=") || !strcmp(s, "|=") || !strcmp(s, "&=") || !strcmp(s, "^=") || !strcmp(s, "%="); }

static void print_decl(P *p, const char *name, Ty ty, const char *init, int has_array, int count){
    char tn[24], gn[48];
    const char *g = glsl_name(name, gn);
    if (ty.base == 'S') put(p->o, "%s %s", p->st[ty.sid].name, g);
    else if (has_array) put(p->o, "%s %s[%d]", glsl_type_name(ty, tn), g, count);
    else put(p->o, "%s %s", glsl_type_name(ty, tn), g);
    if (init) put(p->o, " = %s", init);
}

static void parse_stmt(P *p, int ind);
static void parse_decl_stmt(P *p, int ind, int in_for){
    Lex *L = p->L;
    int is_let = is(L, "let"); L->i++;
    const char *name0 = ident(L); char name[40]; snprintf(name, sizeof name, "%s", name0);
    Ty ty = T_UNK; int have_ty = 0; int has_arr = 0, cnt = 0; Ty elem;
    if (eat(L, ":")){
        if (is(L, "array")){ L->i++; Ty a = parse_array_type(p, &cnt, &elem); ty = elem; has_arr = 1; (void)a; }
        else ty = parse_type(p);
        have_ty = 1;
    }
    char *init = NULL; Ty ity = T_UNK;
    if (eat(L, "=")){ E e = parse_expr(p); init = e.s; ity = e.ty; }
    if (!have_ty) ty = ity;
    if (ty.base == '?'){ fail(L, "cannot infer the type of a declaration"); }
    (void)is_let;
    if (!in_for) pad(p, ind);
    print_decl(p, name, ty, init, has_arr, cnt);
    if (!in_for) put(p->o, ";\n");
    free(init);
    sym_add(p, name, has_arr ? (Ty){ 'A', 0, 0, NULL } : ty);
}
static void parse_stmt(P *p, int ind){
    Lex *L = p->L; Out *o = p->o;
    if (L->bad) return;
    if (is(L, "{")){ L->i++; pad(p, ind); put(o, "{\n"); parse_block(p, ind + 1); pad(p, ind); put(o, "}\n"); return; }
    if (is(L, "let") || is(L, "var")){ parse_decl_stmt(p, ind, 0); expect(L, ";"); return; }
    if (is(L, "return")){
        L->i++; pad(p, ind);
        if (is(L, ";")){ put(o, "return;\n"); L->i++; return; }
        E e = parse_expr(p); put(o, "return %s;\n", e.s); free(e.s); expect(L, ";"); return;
    }
    if (is(L, "discard")){ L->i++; expect(L, ";"); pad(p, ind); put(o, "discard;\n"); return; }
    if (is(L, "if")){
        L->i++; expect(L, "("); E c = parse_expr(p); expect(L, ")");
        pad(p, ind); put(o, "if (%s) {\n", c.s); free(c.s);
        scope_push(p); expect(L, "{"); parse_block(p, ind + 1); scope_pop(p);
        pad(p, ind); put(o, "}");
        while (is(L, "else")){
            L->i++;
            if (is(L, "if")){
                L->i++; expect(L, "("); E c2 = parse_expr(p); expect(L, ")");
                put(o, " else if (%s) {\n", c2.s); free(c2.s);
                scope_push(p); expect(L, "{"); parse_block(p, ind + 1); scope_pop(p);
                pad(p, ind); put(o, "}");
            } else {
                put(o, " else {\n"); scope_push(p); expect(L, "{"); parse_block(p, ind + 1); scope_pop(p); pad(p, ind); put(o, "}");
            }
        }
        put(o, "\n"); return;
    }
    if (is(L, "for")){
        L->i++; expect(L, "("); scope_push(p);
        pad(p, ind); put(o, "for (");
        if (!is(L, ";")) parse_decl_stmt(p, 0, 1);
        expect(L, ";"); put(o, "; ");
        if (!is(L, ";")){ E c = parse_expr(p); put(o, "%s", c.s); free(c.s); }
        expect(L, ";"); put(o, "; ");
        if (!is(L, ")")){
            E lhs = parse_expr(p);
            if (is(L, "++") || is(L, "--")){ put(o, "%s%s", lhs.s, peek(L, 0)->s); L->i++; }
            else if (is_assign_op(peek(L, 0)->s)){ char op[4]; snprintf(op, sizeof op, "%s", peek(L, 0)->s); L->i++; E r = parse_expr(p); put(o, "%s %s %s", lhs.s, op, r.s); free(r.s); }
            free(lhs.s);
        }
        expect(L, ")"); put(o, ") {\n");
        expect(L, "{"); parse_block(p, ind + 1); scope_pop(p);
        pad(p, ind); put(o, "}\n"); return;
    }
    if (is(L, "switch")){
        L->i++; E c = parse_expr(p);
        pad(p, ind); put(o, "switch (%s) {\n", c.s); free(c.s);
        expect(L, "{");
        while (!is(L, "}") && !L->bad && peek(L, 0)->k != K_EOF){
            if (eat(L, "case")){
                E v = parse_expr(p); expect(L, ":"); pad(p, ind + 1); put(o, "case %s: {\n", v.s); free(v.s);
            } else if (eat(L, "default")){ expect(L, ":"); pad(p, ind + 1); put(o, "default: {\n"); }
            else { fail(L, "expected case or default"); break; }
            scope_push(p); expect(L, "{"); parse_block(p, ind + 2); scope_pop(p);
            pad(p, ind + 1); put(o, "} break;\n");
        }
        expect(L, "}"); pad(p, ind); put(o, "}\n"); return;
    }
    /* assignment, increment or a call */
    E lhs = parse_expr(p);
    pad(p, ind);
    if (is(L, "++") || is(L, "--")){ put(o, "%s%s;\n", lhs.s, peek(L, 0)->s); L->i++; }
    else if (is_assign_op(peek(L, 0)->s)){
        char op[4]; snprintf(op, sizeof op, "%s", peek(L, 0)->s); L->i++;
        E r = parse_expr(p); put(o, "%s %s %s;\n", lhs.s, op, r.s); free(r.s);
    } else put(o, "%s;\n", lhs.s);
    free(lhs.s);
    expect(L, ";");
}
static void parse_block(P *p, int ind){      /* after the '{', up to and including the '}' */
    Lex *L = p->L;
    scope_push(p);
    while (!is(L, "}") && !L->bad && peek(L, 0)->k != K_EOF) parse_stmt(p, ind);
    expect(L, "}");
    scope_pop(p);
}

/* ---- top level ---- */
static void parse_struct(P *p){
    Lex *L = p->L; Out *o = p->o;
    L->i++;                                           /* struct */
    const char *name = ident(L);
    if (p->nst >= 16){ fail(L, "too many structs"); return; }
    StructDef *sd = &p->st[p->nst]; memset(sd, 0, sizeof *sd); snprintf(sd->name, 40, "%s", name);
    int sid = p->nst++;
    expect(L, "{");
    put(o, "struct %s {\n", sd->name);
    while (!is(L, "}") && !L->bad && peek(L, 0)->k != K_EOF){
        while (is(L, "@")){ L->i++; ident(L); if (eat(L, "(")){ while (!is(L, ")") && peek(L, 0)->k != K_EOF) L->i++; expect(L, ")"); } }
        const char *mn = ident(L); char mname[40]; snprintf(mname, sizeof mname, "%s", mn);
        expect(L, ":");
        char gn[48];
        if (is(L, "array")){
            L->i++; int cnt; static Ty elems[16]; Ty *el = &elems[sid]; Ty a = parse_array_type(p, &cnt, el);
            char tn[24]; Ty et = *el;
            if (et.base == 'S') put(o, "  %s %s[%d];\n", p->st[et.sid].name, glsl_name(mname, gn), cnt);
            else put(o, "  %s %s[%d];\n", glsl_type_name(et, tn), glsl_name(mname, gn), cnt);
            if (sd->nm < 16){ snprintf(sd->mem[sd->nm], 40, "%s", mname); sd->mty[sd->nm++] = a; }
        } else {
            Ty t = parse_type(p); char tn[24];
            if (t.base == 'S') put(o, "  %s %s;\n", p->st[t.sid].name, glsl_name(mname, gn));
            else put(o, "  %s %s;\n", glsl_type_name(t, tn), glsl_name(mname, gn));
            if (sd->nm < 16){ snprintf(sd->mem[sd->nm], 40, "%s", mname); sd->mty[sd->nm++] = t; }
        }
        eat(L, ",");
    }
    expect(L, "}"); put(o, "};\n");
}

/* NFSU2_NX_TBUF=1: the vertex buffer is read through a buffer texture (texelFetch) instead of a storage buffer.  The yuzu emulator's shader
 * recompiler does not implement the Maxwell LD a Mesa SSBO read becomes (the shader fails to build and the screen stays black); texelFetch is a
 * plain texture fetch it does.  Texture unit TBUF_UNIT carries it. */
static int tbuf_on(void){ const char *e = getenv("NFSU2_NX_TBUF"); return e && *e == '1'; }
static void parse_global_var(P *p, int group, int binding){
    Lex *L = p->L; Out *o = p->o;
    L->i++;                                           /* var */
    int storage = 0, uniform = 0;
    if (eat(L, "<")){
        const char *sp = ident(L); storage = !strcmp(sp, "storage"); uniform = !strcmp(sp, "uniform");
        while (eat(L, ",")) ident(L);
        expect(L, ">");
    }
    char name[40]; snprintf(name, sizeof name, "%s", ident(L));
    expect(L, ":");
    if (is(L, "array")){
        L->i++; int cnt; Ty el; parse_array_type(p, &cnt, &el); char tn[24];
        if (storage && tbuf_on() && el.base == 'U' && el.n == 1){
            snprintf(g_tb_name, sizeof g_tb_name, "%s", name);
            put(o, "layout(binding = 12) uniform usamplerBuffer %s_tb;\n", name);
        } else if (storage) put(o, "layout(std430, binding = %d) readonly buffer %s_buf { %s %s[]; };\n", binding, name, glsl_type_name(el, tn), name);
        static Ty store; store = el; Ty a = { 'A', 0, 0, &store }; sym_add(p, name, a);
    } else if (!strncmp(peek(L, 0)->s, "texture_", 8)){
        const char *tn = ident(L); int cube = strstr(tn, "cube") != NULL;
        if (eat(L, "<")){ parse_type(p); expect(L, ">"); }
        put(o, "layout(binding = %d) uniform %s %s;\n", binding, cube ? "samplerCube" : "sampler2D", name);
        sym_add(p, name, T_UNK);
    } else if (!strcmp(peek(L, 0)->s, "sampler")){
        L->i++; sym_add(p, name, T_UNK);               /* GLSL combines it with its texture: nothing to print */
    } else {
        Ty t = parse_type(p);
        if (uniform && t.base == 'S') put(o, "layout(std140, binding = %d) uniform %s_blk { %s %s; };\n", binding, name, p->st[t.sid].name, name);
        else put(o, "/* var %s */\n", name);
        sym_add(p, name, t);
    }
    expect(L, ";"); (void)group;
}

static void parse_override(P *p){
    Lex *L = p->L; Out *o = p->o;
    L->i++;                                           /* override */
    char name[40]; snprintf(name, sizeof name, "%s", ident(L));
    expect(L, ":"); Ty t = parse_type(p);
    char *init = NULL; if (eat(L, "=")){ E e = parse_expr(p); init = e.s; }
    expect(L, ";");
    if (!strcmp(name, "ALPHA_FUNC")) put(o, "uniform uint ALPHA_FUNC;\n");
    else { char tn[24]; put(o, "const %s %s = %s;\n", glsl_type_name(t, tn), name, init ? init : "0"); }
    free(init);
    sym_add(p, name, t);
}

static void parse_function(P *p, int is_vertex, int is_frag){
    Lex *L = p->L; Out *o = p->o;
    L->i++;                                           /* fn */
    char name[40]; snprintf(name, sizeof name, "%s", ident(L));
    expect(L, "(");
    char params[1024]; params[0] = 0;
    scope_push(p);
    int np = 0;
    struct { char name[40]; char builtin[24]; Ty ty; } ps[8];
    while (!is(L, ")") && !L->bad && np < 8){
        ps[np].builtin[0] = 0;
        while (is(L, "@")){
            L->i++; const char *at = ident(L);
            if (eat(L, "(")){ const char *arg = ident(L); if (!strcmp(at, "builtin")) snprintf(ps[np].builtin, 24, "%s", arg); while (eat(L, ",")) ident(L); expect(L, ")"); }
        }
        snprintf(ps[np].name, 40, "%s", ident(L)); expect(L, ":"); ps[np].ty = parse_type(p);
        char tn[24], gn[48];
        if (ps[np].ty.base == 'S') snprintf(params + strlen(params), sizeof params - strlen(params), "%s%s %s", np ? ", " : "", p->st[ps[np].ty.sid].name, glsl_name(ps[np].name, gn));
        else snprintf(params + strlen(params), sizeof params - strlen(params), "%s%s %s", np ? ", " : "", glsl_type_name(ps[np].ty, tn), glsl_name(ps[np].name, gn));
        sym_add(p, ps[np].name, ps[np].ty);
        np++;
        if (!eat(L, ",")) break;
    }
    expect(L, ")");
    Ty ret = { 'F', 0, 0, 0 }; ret.n = 0; ret.base = 'v';       /* void */
    if (eat(L, "->")){
        while (is(L, "@")){ L->i++; ident(L); if (eat(L, "(")){ while (!is(L, ")") && peek(L, 0)->k != K_EOF) L->i++; expect(L, ")"); } }
        ret = parse_type(p);
    }
    char rtn[48], tn[24];
    if (ret.base == 'v') snprintf(rtn, sizeof rtn, "void");
    else if (ret.base == 'S') snprintf(rtn, sizeof rtn, "%s", p->st[ret.sid].name);
    else snprintf(rtn, sizeof rtn, "%s", glsl_type_name(ret, tn));
    if (p->nfn < 64){ snprintf(p->fns[p->nfn].name, 40, "%s", name); p->fns[p->nfn].ret = ret.base == 'v' ? T_UNK : ret; p->nfn++; }
    char gn[48];
    put(o, "%s %s(%s) {\n", rtn, glsl_name(name, gn), params);
    expect(L, "{"); parse_block(p, 1); scope_pop(p);
    put(o, "}\n");
    if (is_vertex || is_frag){
        snprintf(p->entry, 40, "%s", name); p->is_frag = is_frag; p->nentry_params = np;
        for (int i = 0; i < np; i++){ snprintf(p->entry_params[i], 40, "%s", ps[i].name); snprintf(p->entry_pbuiltin[i], 24, "%s", ps[i].builtin); }
    }
}

int wgsl_to_glsl(const char *wgsl, char *out, size_t cap, int *is_fragment, char *err, size_t errcap){
    g_tb_name[0] = 0;
    Lex L = { 0 }; L.err = err; L.errcap = errcap;
    if (err && errcap) err[0] = 0;
    if (lex(wgsl, &L)){ free(L.t); return -1; }
    static P p0; P *p = &p0; memset(p, 0, sizeof *p);
    Out o = { out, cap, 0, 1 }; if (cap) out[0] = 0;
    p->L = &L; p->o = &o;
    put(&o, "#version 430 core\n");
    put(&o, "uniform int u_kb;\n");
    /* the interstage struct is plain here: the entry's wrapper moves it to and from the stage's in/out variables */
    while (peek(&L, 0)->k != K_EOF && !L.bad){
        int group = -1, binding = -1, vertex = 0, frag = 0;
        while (is(&L, "@")){
            L.i++; const char *at = ident(&L);
            if (!strcmp(at, "vertex")) vertex = 1;
            else if (!strcmp(at, "fragment")) frag = 1;
            else if (eat(&L, "(")){
                const Tok *a = peek(&L, 0); int v = atoi(a->s); L.i++;
                if (!strcmp(at, "group")) group = v; else if (!strcmp(at, "binding")) binding = v;
                expect(&L, ")");
            }
        }
        if (is(&L, "struct")) parse_struct(p);
        else if (is(&L, "var")) parse_global_var(p, group, binding);
        else if (is(&L, "override")) parse_override(p);
        else if (is(&L, "fn")) parse_function(p, vertex, frag);
        else if (is(&L, "const")){ fail(&L, "const is not supported"); }
        else { fail(&L, "unexpected top-level token"); }
    }
    int rc = 0;
    if (!L.bad && p->entry[0]){
        /* the entry point's wrapper: stage in/out variables around the WGSL function */
        if (!p->is_frag){
            put(&o, "out vec4 v_col0; out vec4 v_col1; out vec4 v_uv0; out vec4 v_uv1; out vec4 v_uv2; out vec4 v_uv3; out float v_fog;\n");
            char args[256]; args[0] = 0;
            for (int i = 0; i < p->nentry_params; i++){
                const char *v = !strcmp(p->entry_pbuiltin[i], "vertex_index") ? "uint(gl_VertexID)" : !strcmp(p->entry_pbuiltin[i], "instance_index") ? "uint(u_kb)" : "0u";
                snprintf(args + strlen(args), sizeof args - strlen(args), "%s%s", i ? ", " : "", v);
            }
            put(&o, "void main() {\n  VOut o = %s(%s);\n  gl_Position = o.pos;\n  v_col0 = o.col0; v_col1 = o.col1; v_uv0 = o.uv0; v_uv1 = o.uv1; v_uv2 = o.uv2; v_uv3 = o.uv3; v_fog = o.fog;\n}\n", p->entry, args);
        } else {
            put(&o, "in vec4 v_col0; in vec4 v_col1; in vec4 v_uv0; in vec4 v_uv1; in vec4 v_uv2; in vec4 v_uv3; in float v_fog;\nout vec4 o_color;\n");
            put(&o, "void main() {\n  VOut i;\n  i.pos = gl_FragCoord;\n  i.col0 = v_col0; i.col1 = v_col1; i.uv0 = v_uv0; i.uv1 = v_uv1; i.uv2 = v_uv2; i.uv3 = v_uv3; i.fog = v_fog;\n  o_color = %s(i);\n}\n", p->entry);
        }
        if (is_fragment) *is_fragment = p->is_frag;
    } else if (!L.bad){ if (err && errcap) snprintf(err, errcap, "no entry point"); rc = -3; }
    if (L.bad) rc = -2;
    if (!o.ok){ if (err && errcap) snprintf(err, errcap, "output buffer too small"); rc = -4; }
    free(L.t);
    return rc;
}
