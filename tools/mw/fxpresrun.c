/* fxpresrun.c -- implementation A's side of the preshader evaluator checks
 * (docs/mw/fx-preshader.md §6): runtime/mw/fxpres.c on the canonical cases.
 *
 *   clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
 *         -Iruntime/mw -o out/mw/fx/bin/fxpresrun tools/mw/fxpresrun.c runtime/mw/fxpres.c \
 *         runtime/mw/fxmodel.c runtime/mw/fx2.c -lm
 *   out/mw/fx/bin/fxpresrun vm <cases.txt> <out.txt|->               ("fxpcase 1" -> "fxpres 1", §6.2)
 *   out/mw/fx/bin/fxpresrun effect <rounds.txt> <out.txt|-> <NAME>.fxo...   ("fxpround 1" -> "fxpeff 1", §6.3:
 *                                                                       SetValue and the typed setters on the
 *                                                                       model, then every pass's preshader states)
 *   out/mw/fx/bin/fxpresrun fuzz <copies> <NAME>.fxo...               (§6.5)
 *
 * tools/fxpres.py (implementation B) writes the same two texts; the two
 * must be the same byte for byte.  tools/mw/fxoracle.c writes the DLL's
 * side, which tools/mw/fxpres_compare.py checks against these.
 */
#include "fxpres.h"
#include "fxtext.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *fmt, ...){
    va_list a; va_start(a, fmt);
    fputs("fxpresrun: ", stderr); vfprintf(stderr, fmt, a); fputc('\n', stderr);
    va_end(a); exit(1);
}
static void *xalloc(size_t n){ void *p = calloc(1, n ? n : 1); if (!p) die("out of memory"); return p; }
static uint8_t *slurp(const char *path, uint32_t *size){
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = xalloc(n > 0 ? (size_t)n : 1);
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n){ fclose(f); free(b); return NULL; }
    fclose(f); *size = (uint32_t)n; return b;
}

/* ---- a whitespace tokenizer over a whole text file ---- */
typedef struct { char *p; const char *path; unsigned line; int nl; } lex;
/* the next field, "\n" at the end of a line, NULL at the end */
static char *tok(lex *L){
    if (L->nl){ L->nl = 0; L->line++; return "\n"; }
    while (*L->p == ' ' || *L->p == '\t' || *L->p == '\r') L->p++;
    if (!*L->p) return NULL;
    if (*L->p == '\n'){ L->p++; L->line++; return "\n"; }
    char *s = L->p;
    while (*L->p && *L->p != ' ' && *L->p != '\t' && *L->p != '\r' && *L->p != '\n') L->p++;
    if (*L->p == '\n') L->nl = 1;
    if (*L->p) *L->p++ = 0;
    return s;
}
static char *need(lex *L){ char *t = tok(L); if (!t || !strcmp(t, "\n")) die("%s:%u: missing field", L->path, L->line); return t; }
static void eol(lex *L){ char *t = tok(L); if (t && strcmp(t, "\n")) die("%s:%u: extra field %s", L->path, L->line, t); }
static uint64_t num(lex *L, const char *t, int hex){
    char *e; unsigned long long v = strtoull(t, &e, hex ? 16 : 10);
    if (*e || !*t) die("%s:%u: not a number: %s", L->path, L->line, t);
    return v;
}
static void expect(lex *L, const char *want){
    char *t = need(L);
    if (strcmp(t, want)) die("%s:%u: %s where %s was expected", L->path, L->line, t, want);
}

/* ---- vm: "fxpcase 1" -> "fxpres 1" ---- */
static uint64_t *table(lex *L, uint32_t *n, uint32_t *mask){
    *n = (uint32_t)num(L, need(L), 0);
    expect(L, "mask");
    *mask = (uint32_t)num(L, need(L), 0);
    uint64_t *t = xalloc((size_t)*n * 8);
    for (uint32_t i = 0; i < *n; i++) t[i] = num(L, need(L), 1);
    eol(L);
    return t;
}
static void put_table(FILE *f, const char *what, const uint64_t *t, uint32_t n){
    fprintf(f, "  %s", what);
    for (uint32_t i = 0; i < n; i++) fprintf(f, " %016llx", (unsigned long long)t[i]);
    fputc('\n', f);
}
static int run_vm(const char *in, const char *outp){
    uint32_t sz; char *text = (char *)slurp(in, &sz);
    if (!text) die("cannot read %s", in);
    text = realloc(text, (size_t)sz + 1); text[sz] = 0;
    lex L = { text, in, 1, 0 };
    FILE *f = strcmp(outp, "-") ? fopen(outp, "w") : stdout;
    if (!f) die("cannot write %s", outp);
    expect(&L, "fxpcase"); expect(&L, "1"); eol(&L);
    fprintf(f, "fxpres 1\n");
    unsigned long ncase = 0;
    for (;;){
        char *t = tok(&L);
        while (t && !strcmp(t, "\n")) t = tok(&L);
        if (!t) break;
        if (strcmp(t, "case")) die("%s:%u: %s where case was expected", in, L.line, t);
        char name[256]; snprintf(name, sizeof name, "%s", need(&L)); eol(&L);
        int pc = 0; uint32_t nb = 0; uint8_t *blob = NULL;
        uint64_t *c = NULL, *o = NULL, *v = NULL; uint32_t nc = 0, cm = 0, no = 0, om = 0, nv = 0, vm = 0; int havev = 0;
        int unknown = 0;
        for (;;){
            t = need(&L);
            if (!strcmp(t, "end")){ eol(&L); break; }
            if (!strcmp(t, "pc")){ pc = (int)num(&L, need(&L), 0); eol(&L); }
            else if (!strcmp(t, "temps")){ expect(&L, "unknown"); eol(&L); unknown = 1; }   /* as fxp_eval runs */
            else if (!strcmp(t, "run")){ num(&L, need(&L), 0); eol(&L); }      /* the oracle's: A runs every case it loads */
            else if (!strcmp(t, "blob")){
                nb = (uint32_t)num(&L, need(&L), 0);
                blob = xalloc(nb);
                for (uint32_t i = 0; i < nb; i += 4){
                    uint32_t w = (uint32_t)num(&L, need(&L), 1);
                    for (uint32_t k = 0; k < 4 && i + k < nb; k++) blob[i + k] = (uint8_t)(w >> 8 * k);
                }
                eol(&L);
            }
            else if (!strcmp(t, "c")) c = table(&L, &nc, &cm);
            else if (!strcmp(t, "o")) o = table(&L, &no, &om);
            else if (!strcmp(t, "v")){ v = table(&L, &nv, &vm); havev = 1; }
            else die("%s:%u: unknown line %s", in, L.line, t);
        }
        fxp_prog *p = NULL;
        int err = fxp_load(&p, blob ? blob : (const uint8_t *)"", nb);
        fprintf(f, "case %s load ", name);
        if (err) fprintf(f, "%s %s\n", fxp_refused_by_d3dx(err) ? "fail" : "unsupported", fxp_err_name(err));
        else {
            fprintf(f, "ok\n  class %s\n", fxp_approx(p) ? "approx" : "exact");
            fxp_regs *x = xalloc(sizeof *x);
            x->c = c; x->csize = nc; x->cmask = cm;
            if (havev){ x->v = v; x->vsize = nv; x->vmask = vm; }
            x->o = o; x->osize = no; x->omask = om;
            x->temps_unknown = unknown;
            x->pc = pc;
            int rerr = fxp_run(p, x);
            fprintf(f, "  run %s\n", rerr ? fxp_err_name(rerr) : "ok");
            put_table(f, "o", o, no);
            fprintf(f, "  r");
            int any = 0;
            for (uint32_t i = 0; i < FXP_NTEMP; i++)
                if (x->r[i]){ fprintf(f, " %u=%016llx", i, (unsigned long long)x->r[i]); any = 1; }
            fprintf(f, "%s\n", any ? "" : " -");
            free(x);
            fxp_free(p);
        }
        free(blob); free(c); free(o); free(v);
        ncase++;
    }
    fprintf(f, "end\n");
    if (f != stdout) fclose(f);
    free(text);
    fprintf(stderr, "fxpresrun vm: %lu cases\n", ncase);
    return 0;
}

/* ---- effect: "fxpround 1" -> "fxpeff 1" ---- */
typedef struct { const char *name; uint8_t *buf; uint32_t size; fx2_effect fx; fxm_effect *m; fxp_effect *x; } eff;
static int by_name(const void *a, const void *b){ return strcmp(((const eff *)a)->name, ((const eff *)b)->name); }

/* the device call that carries state s's value (§6.3), and where in it */
static void key(FILE *f, const fx2_state *s, uint32_t stage){
    const fx2_state_info *si = fx2_state_get(s->op);
    switch (si ? si->cls : FX2_NSCLASSES){
    case FX2_SC_RENDER: fprintf(f, "rs %u", si->d3d); break;
    case FX2_SC_STAGE: fprintf(f, "tss %u %u", s->index, si->d3d); break;
    case FX2_SC_SAMPLERSTATE: fprintf(f, "ss %u %u", stage, si->d3d); break;
    case FX2_SC_TRANSFORM: fprintf(f, "transform %u 0", si->d3d + s->index); break;
    case FX2_SC_MATERIAL: fprintf(f, "material %u", si->d3d < 4 ? 4 * si->d3d : 16); break;
    case FX2_SC_LIGHT: {
        static const uint8_t off[13] = { 0, 1, 5, 9, 13, 16, 19, 20, 21, 22, 23, 24, 25 };
        if (si->d3d == 13) fprintf(f, "lightenable %u", s->index);
        else fprintf(f, "light %u %u", s->index, si->d3d < 13 ? off[si->d3d] : 0);
        break;
    }
    default: fprintf(f, "other %s", si ? fx2_sclass_name(si->cls) : "?"); break;
    }
}
static const void *g_reached[4096]; static uint32_t g_nreached;   /* the preshaders evaluated */
static void state_line(FILE *f, eff *e, const fx2_state *s, uint32_t k, uint32_t stage, int pc, const char *lead){
    fxm_source src = fxm_state_source(e->m, s);
    if (src.src != FXM_S_PRESHADER) return;
    uint32_t i = 0;
    while (i < g_nreached && g_reached[i] != src.prog) i++;
    if (i == g_nreached && g_nreached < 4096) g_reached[g_nreached++] = src.prog;
    const fx2_state_info *si = fx2_state_get(s->op);
    fprintf(f, "%sstate %u %s[%u] -> ", lead, k, si ? si->name : "?", s->index);
    key(f, s, stage);
    uint32_t v[64], n = 0;
    int err = fxp_state_value(e->x, s, pc, v, 64, &n);
    if (err){ fprintf(f, " = error %s\n", fxp_err_name(err)); return; }
    fputs(" =", f);
    for (uint32_t i = 0; i < n; i++) fprintf(f, " %08x", v[i]);
    fputc('\n', f);
}
static void pass_states(FILE *f, eff *e, uint32_t ti, uint32_t pi, int pc){
    const fx2_pass *ps = &e->fx.techs[ti].passes[pi];
    for (uint32_t k = 0; k < ps->nstates; k++) state_line(f, e, &ps->states[k], k, 0, pc, "    ");
    /* the sampler blocks the pass's applied shaders bind (fx-model §3.4): stage reg + j for j < used */
    for (uint32_t k = 0; k < ps->nstates; k++){
        fxm_source src = fxm_state_source(e->m, &ps->states[k]);
        if (src.src != FXM_S_SHADER || !src.prog) continue;
        const fx2_state_info *si = fx2_state_get(ps->states[k].op);
        if (!si || (si->cls != FX2_SC_VSHADER && si->cls != FX2_SC_PSHADER)) continue;
        const fxm_prog *pr = src.prog;
        for (uint32_t c = 0; c < pr->nread; c++){
            const fxm_const *mc = &pr->consts[c];
            if (mc->why || mc->layout != FXM_L_SAMPLER) continue;
            for (uint32_t j = 0; j < mc->used && j < mc->nleaves; j++){
                const fxm_node *nd = fxm_node_get(e->m, mc->leaves[j].id);
                if (!nd || !nd->block) continue;
                for (uint32_t b = 0; b < nd->block->nstates; b++){
                    char lead[128];
                    snprintf(lead, sizeof lead, "    sampler %.*s stage %u ", (int)nd->path.n, (const char *)nd->path.p,
                             mc->stage + j);
                    state_line(f, e, &nd->block->states[b], b, mc->stage + j, pc, lead);
                }
            }
        }
    }
}
static int run_effect(const char *in, const char *outp, int nfiles, char **files){
    eff *E = xalloc(sizeof *E * (size_t)nfiles);
    fxm_pool *pool = fxm_pool_new();
    if (!pool) die("out of memory");
    for (int i = 0; i < nfiles; i++){
        const char *b = strrchr(files[i], '/'); b = b ? b + 1 : files[i];
        size_t l = strlen(b); if (l > 4 && !strcmp(b + l - 4, ".fxo")) l -= 4;
        char *nm = xalloc(l + 1); memcpy(nm, b, l);
        E[i].name = nm;
        if (!(E[i].buf = slurp(files[i], &E[i].size))) die("cannot read %s", files[i]);
    }
    qsort(E, (size_t)nfiles, sizeof *E, by_name);
    char err[256];
    for (int i = 0; i < nfiles; i++){
        if (fx2_parse(&E[i].fx, E[i].buf, E[i].size, err, sizeof err)) die("%s: %s", E[i].name, err);
        if (fxm_effect_new(&E[i].m, &E[i].fx, pool, err, sizeof err)) die("%s: %s", E[i].name, err);
        if (fxp_effect_new(&E[i].x, E[i].m)) die("out of memory");
    }
    uint32_t sz; char *text = (char *)slurp(in, &sz);
    if (!text) die("cannot read %s", in);
    text = realloc(text, (size_t)sz + 1); text[sz] = 0;
    lex L = { text, in, 1, 0 };
    FILE *f = strcmp(outp, "-") ? fopen(outp, "w") : stdout;
    if (!f) die("cannot write %s", outp);
    expect(&L, "fxpround"); expect(&L, "1"); eol(&L);
    fprintf(f, "fxpeff 1\n");
    int pc = 24, next_pc = 24; unsigned long nround = 0, nset = 0, ncall = 0;
    char round[64] = "";
    for (;;){
        char *t = tok(&L);
        while (t && !strcmp(t, "\n")) t = tok(&L);
        if (!t) break;
        if (!strcmp(t, "set")){
            char *en = need(&L), *path = need(&L);
            uint32_t n = (uint32_t)num(&L, need(&L), 0), *w = xalloc(4 * (size_t)n);
            for (uint32_t i = 0; i < n; i++) w[i] = (uint32_t)num(&L, need(&L), 1);
            eol(&L);
            eff *e = NULL;
            for (int i = 0; i < nfiles && !e; i++) if (!strcmp(E[i].name, en)) e = &E[i];
            if (!e) die("%s:%u: no effect %s", in, L.line, en);
            uint32_t id = fxm_by_name(e->m, 0, (const uint8_t *)path, (uint32_t)strlen(path));
            if (!id) die("%s:%u: %s has no parameter %s", in, L.line, en, path);
            uint32_t hr = fxm_set_value(e->m, id, w, 4 * n, NULL);
            if (hr) die("%s:%u: SetValue %s.%s: 0x%08x", in, L.line, en, path, hr);
            free(w); nset++;
            continue;
        }
        if (!strcmp(t, "call")){                    /* a typed setter of the model (§6.3) */
            char *setter = need(&L), *en = need(&L), *path = need(&L);
            uint32_t count = (uint32_t)num(&L, need(&L), 0), n = (uint32_t)num(&L, need(&L), 0);
            uint32_t *w = xalloc(4 * ((size_t)n + 16));
            for (uint32_t i = 0; i < n; i++) w[i] = (uint32_t)num(&L, need(&L), 1);
            eol(&L);
            eff *e = NULL;
            for (int i = 0; i < nfiles && !e; i++) if (!strcmp(E[i].name, en)) e = &E[i];
            if (!e) die("%s:%u: no effect %s", in, L.line, en);
            uint32_t id = fxm_by_name(e->m, 0, (const uint8_t *)path, (uint32_t)strlen(path));
            if (!id) die("%s:%u: %s has no parameter %s", in, L.line, en, path);
            uint32_t hr = 0, need_dw = !strcmp(setter, "SetVector") ? 4 : !strncmp(setter, "SetMatrix", 9) &&
                                   !strstr(setter, "Array") ? 16 : 1;
            if (n < need_dw) die("%s:%u: %s takes %u dwords", in, L.line, setter, need_dw);
            fxm_set_x87_pc(e->m, pc);                   /* the round's precision (fx-model §2.5) */
            if (!strcmp(setter, "SetBool")) hr = fxm_set_bool(e->m, id, w[0], NULL);
            else if (!strcmp(setter, "SetInt")) hr = fxm_set_int(e->m, id, w[0], NULL);
            else if (!strcmp(setter, "SetFloat")) hr = fxm_set_float(e->m, id, w[0], NULL);
            else if (!strcmp(setter, "SetBoolArray")) hr = fxm_set_array(e->m, id, FX2_T_BOOL, w, count, NULL);
            else if (!strcmp(setter, "SetIntArray")) hr = fxm_set_array(e->m, id, FX2_T_INT, w, count, NULL);
            else if (!strcmp(setter, "SetFloatArray")) hr = fxm_set_array(e->m, id, FX2_T_FLOAT, w, count, NULL);
            else if (!strcmp(setter, "SetVector")) hr = fxm_set_vector(e->m, id, w, NULL);
            else if (!strcmp(setter, "SetVectorArray")) hr = fxm_set_vector_array(e->m, id, w, count, NULL);
            else if (!strcmp(setter, "SetMatrix")) hr = fxm_set_matrix(e->m, id, w, 0, NULL);
            else if (!strcmp(setter, "SetMatrixTranspose")) hr = fxm_set_matrix(e->m, id, w, 1, NULL);
            else if (!strcmp(setter, "SetMatrixArray")) hr = fxm_set_matrix_array(e->m, id, w, count, 0, NULL);
            else if (!strcmp(setter, "SetMatrixTransposeArray")) hr = fxm_set_matrix_array(e->m, id, w, count, 1, NULL);
            else die("%s:%u: no setter %s", in, L.line, setter);
            fprintf(f, "call %s %s %s = %08x\n", setter, en, path, hr);
            free(w); ncall++;
            continue;
        }
        if (!strcmp(t, "round") || !strcmp(t, "end")){
            int last = !strcmp(t, "end");
            char next[64] = "";
            if (!last){ snprintf(next, sizeof next, "%s", need(&L)); expect(&L, "pc"); next_pc = (int)num(&L, need(&L), 0); }
            eol(&L);
            if (round[0]){                          /* the round before: every pass of every technique */
                fprintf(f, "round %s pc %d\n", round, pc);
                for (int i = 0; i < nfiles; i++){
                    fprintf(f, "effect %s\n", E[i].name);
                    for (uint32_t ti = 0; ti < E[i].fx.ntechs; ti++)
                        for (uint32_t pi = 0; pi < E[i].fx.techs[ti].npasses; pi++){
                            fprintf(f, "  tech %u pass %u\n", ti, pi);
                            pass_states(f, &E[i], ti, pi, pc);
                        }
                }
                nround++;
            }
            snprintf(round, sizeof round, "%s", next);
            pc = next_pc;
            if (last) break;
            continue;
        }
        die("%s:%u: unknown line %s", in, L.line, t);
    }
    fprintf(f, "end\n");
    if (f != stdout) fclose(f);
    free(text);
    for (int i = 0; i < nfiles; i++){ fxp_effect_free(E[i].x); fxm_effect_free(E[i].m, NULL, NULL); fx2_free(&E[i].fx); }
    fxm_pool_free(pool);
    for (int i = 0; i < nfiles; i++){ free(E[i].buf); free((void *)E[i].name); }
    free(E);
    fprintf(stderr, "fxpresrun effect: %lu rounds, %lu sets, %lu calls, %u preshaders reached through passes and bound "
            "samplers\n", nround, nset, ncall, g_nreached);
    return 0;
}

/* ---- fuzz (§6.5): truncated, corrupted and made-up preshaders, loaded and run under the sanitizers ---- */
static uint64_t g_rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void){ g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (uint32_t)(g_rs >> 32); }
static const uint64_t k_vals[] = { 0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000001ull, 0x7FF0000000000001ull, 1, 0x000FFFFFFFFFFFFFull,
    0x4330000000000000ull, 0x41E0000000000000ull, 0xC1E0000000000000ull, 0x43E0000000000000ull, 0x3FE0000000000000ull };
static uint64_t rval(void){ return rnd() & 1 ? k_vals[rnd() % (sizeof k_vals / sizeof k_vals[0])] : (uint64_t)rnd() << 32 | rnd(); }
static unsigned long g_loaded, g_refused, g_runs, g_range;
static void try_blob(const uint8_t *b, uint32_t n){
    fxp_prog *p = NULL;
    int err = fxp_load(&p, b, n);
    if (err){ g_refused++; return; }
    g_loaded++;
    for (int pass = 0; pass < 2; pass++){
        fxp_regs *x = xalloc(sizeof *x);
        uint32_t nc = 1u << (rnd() % 7), no = 1u << (rnd() % 7);
        uint64_t *c = xalloc(8 * (size_t)nc), *o = xalloc(8 * (size_t)no);
        for (uint32_t i = 0; i < nc; i++) c[i] = rval();
        for (uint32_t i = 0; i < FXP_NTEMP; i++) x->r[i] = rnd() & 3 ? 0 : rval();
        x->c = c; x->csize = nc; x->cmask = rnd() & 7 ? nc - 1 : rnd();
        x->o = o; x->osize = no; x->omask = rnd() & 7 ? no - 1 : rnd();
        static const int pcs[3] = { 24, 53, 64 };
        x->pc = pcs[rnd() % 3];
        x->temps_unknown = (int)(rnd() & 1);
        if (fxp_run(p, x)) g_range++;
        g_runs++;
        free(c); free(o); free(x);
    }
    fxp_free(p);
}
static int run_fuzz(unsigned long copies, int nfiles, char **files){
    unsigned long blobs = 0;
    for (int i = 0; i < nfiles; i++){
        uint32_t sz; uint8_t *buf = slurp(files[i], &sz);
        if (!buf) die("cannot read %s", files[i]);
        fx2_effect fx; char err[256];
        if (fx2_parse(&fx, buf, sz, err, sizeof err)) die("%s: %s", files[i], err);
        for (uint32_t r = 0; r < fx.nres; r++){
            const fx2_res *res = &fx.res[r];
            if (res->usage || res->size < 4 || fx2_rd32(res->data) >> 16 != 0x4658) continue;
            blobs++;
            try_blob(res->data, res->size);
            for (unsigned long k = 0; k < copies; k++){
                uint32_t n = res->size;
                uint8_t *b = xalloc(n + 64);
                memcpy(b, res->data, n);
                switch (rnd() % 6){
                case 0: n = rnd() % (n + 1); break;                               /* truncated */
                case 1: for (int j = 0; j < 1 + (int)(rnd() % 4); j++) b[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;
                case 2: { uint32_t at = rnd() % (n / 4) * 4; uint32_t w = rnd() % 12;   /* a small dword: tables, flags */
                          memcpy(b + at, &w, 4); break; }
                case 3: { uint32_t at = rnd() % (n / 4) * 4; uint32_t w = (rnd() % 0x800) << 20 | (rnd() % 6);
                          if (rnd() & 1) w |= 0x80000000u; memcpy(b + at, &w, 4); break; }      /* an instruction token */
                case 4: { uint32_t at = rnd() % (n / 4) * 4, w = rnd(); memcpy(b + at, &w, 4); break; }
                default: for (uint32_t j = 0; j < 64; j++) b[n + j] = (uint8_t)rnd(); n += rnd() % 64; break;
                }
                uint8_t *exact = xalloc(n);                                        /* exactly n bytes for ASan */
                memcpy(exact, b, n);
                try_blob(exact, n);
                free(exact); free(b);
            }
        }
        fx2_free(&fx); free(buf);
    }
    /* made-up programs: random instructions with in-range operands mostly */
    for (unsigned long k = 0; k < copies * 20; k++){
        uint32_t w[512], n = 0, nins = 1 + rnd() % 8;
        w[n++] = 0x46580200u;
        uint32_t nl = rnd() % 4;
        w[n++] = 0xFFFEu | (2 + 2 * nl) << 16; w[n++] = 0x54494C43u; w[n++] = nl;
        for (uint32_t i = 0; i < nl; i++){ uint64_t v = rval(); w[n++] = (uint32_t)v; w[n++] = (uint32_t)(v >> 32); }
        uint32_t fx = n; w[n++] = 0xFFFEu; w[n++] = 0x434C5846u; w[n++] = nins;
        static const uint16_t ops[] = { 0x100, 0x101, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x10A, 0x10B,
            0x10C, 0x200, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x208, 0x300, 0x301, 0x500, 0x70E, 0x000, 0x102 };
        for (uint32_t i = 0; i < nins && n < 400; i++){
            uint32_t op = ops[rnd() % (sizeof ops / sizeof ops[0])], nin = op >> 8;
            if (op == 0x70E) nin = 6 + 2 * (rnd() & 1);
            if (op == 0x500) nin = 2;
            w[n++] = op << 20 | (rnd() % 5) | (rnd() % 8 ? 0 : 0x80000000u);
            w[n++] = nin;
            for (uint32_t k2 = 0; k2 <= nin && n < 480; k2++){
                int out = k2 == nin;
                uint32_t rel = !out && rnd() % 6 == 0;
                w[n++] = rel;
                if (rel){ w[n++] = 1 + rnd() % 7; w[n++] = rnd() % 16; }
                static const uint32_t in_t[] = { 1, 2, 2, 4, 7, 7 }, out_t[] = { 4, 5, 6, 7 };
                w[n++] = out ? out_t[rnd() % 4] : in_t[rnd() % 6];
                w[n++] = rnd() % 24;
            }
        }
        w[n++] = 0xF0F0F0F0u; w[n++] = 0x0F0F0F0Fu;
        w[fx] = 0xFFFEu | (n - fx - 1) << 16;
        w[n++] = FX2_END;
        uint8_t *b = xalloc(4 * (size_t)n);
        memcpy(b, w, 4 * (size_t)n);
        try_blob(b, 4 * n);
        free(b);
        blobs++;
    }
    printf("fxpresrun fuzz: %lu source programs, %lu loaded, %lu refused, %lu runs (%lu out of range), no sanitizer report\n",
           blobs, g_loaded, g_refused, g_runs, g_range);
    return 0;
}

int main(int argc, char **argv){
    if (argc >= 4 && !strcmp(argv[1], "vm")) return run_vm(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "effect")) return run_effect(argv[2], argv[3], argc - 4, argv + 4);
    if (argc >= 4 && !strcmp(argv[1], "fuzz")) return run_fuzz(strtoul(argv[2], NULL, 10), argc - 3, argv + 3);
    fprintf(stderr, "usage: fxpresrun vm <cases.txt> <out.txt|->\n"
                    "       fxpresrun effect <rounds.txt> <out.txt|-> <NAME>.fxo...\n"
                    "       fxpresrun fuzz <copies> <NAME>.fxo...\n");
    return 2;
}
