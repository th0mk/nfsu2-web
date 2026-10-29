/* Most Wanted's game flow and front end, as core's logs and the test
 * scripts read them (runtime/game.h flow, top_screen, on_test_frame,
 * fe_object; runtime/autotest.c waitmem, waitfng, tapuntil, $O and the
 * [fng] lines).  It
 * only reads.  The addresses are speed.exe v1.3's, from its code (GAME.md
 * 13.1, 13.2).
 *
 * GameFlowManager's state is the dword 0x925E90, numbered as UG2's: 1
 * loading the front end, 2 unloading it, 3 in the front end, 5 loading a
 * track, 6 racing, 7 unloading the track, 8 unloading the region, 9 leaving.
 * Its writers: 0x664B80 (1), 0x667010 (2), 0x64B6D0 (3), 0x666FA0 (5),
 * 0x6627E0 (6), 0x6617E0 (7), 0x664250 (8), 0x6597F0 (9); 4 comes through a
 * register.
 *
 * The front end: cFEng is [0x91CADC] and its FEngine [cFEng].  FEngine+0xD8
 * is the list of active packages (count +0xDC, head +0xE0, tail +0xE4) and
 * +0xE8 the idle ones (head +0xF0): FindPackage looks a name up in the first,
 * then the second (0x5B5B70 and 0x5B7580, both 0x5B5B10), and UnloadPackage
 * (0x5BE6A0) moves a package from the first to the second.  A package's next
 * is at +4, its previous at +8, its name at +0xC and the name's hash at
 * +0x10.  FEngine's render (0x5BE800) keeps the active list sorted by the
 * packages' priority (+0x1C) and draws it from head to tail, so the tail is
 * the package on top, the top screen (UG2's GetTopPackage 0x50C8E0 reads
 * its FEngine's +0xE4 too).
 *
 * The screens the game brings up: UG2 has them from its debug printf ("Init
 * Package %s", "SwitchFEngPackages from %s to %s", overrides.c), but MW's
 * retail exe has neither string.  So at every Present (on_test_frame) the
 * active list is walked and each package that was not in it the frame before
 * is noted (autotest_note_screen), which gives MW the same screen lines,
 * waitfng and tapuntil matches.
 *
 * A package's objects, for the scripts' $O(PACKAGE:HASH) (game.h fe_object),
 * as FEPackage::ForAllObjects (0x5B1560) walks them: the first at the
 * package's +0x4C, the next at +4; an object of type 5 (+0x18), a group,
 * holds its own list from +0x64 (0x5B1510).  An object's name hash is at
 * +0x10 (the splash's prompt 0xC6CC55A4, whose script 0x5A3147 starts) and
 * its data at +0x2C, which begins with its colour as four dwords, blue,
 * green, red and alpha: a script fading an object moves that alpha (seen in
 * MW_LS_Splash.fng's objects, frame by frame).
 *
 * Within a package (game.h fe_match: the scripts' names PACKAGE:LABEL,
 * PACKAGE@HASH, PACKAGE'TEXT, PACKAGE^TEXT; mw_fe_match below).  MW's
 * dialogs live inside MC_Main.fng, the only package that changes while they
 * come and go (the NFSU2_FNG_LOG=2 package list), so a dialog is told by what
 * its package shows and by the package's state, all seen in headless runs of
 * tests/mw/scripts and named from them:
 *   - FEPackage +0x28, a byte, takes input: 0 while the package comes in or
 *     goes, 1 while it answers keys.  MC_Main.fng's drops to 0 in the frame a
 *     RETURN answers a dialog and comes back once the next dialog is in.
 *     FEngine's thiscall 0x5B07F0 (package, flag) writes it.
 *   - FEPackage +0x8C, the highlighted object (FEng's current button): LEFT on
 *     a Yes/No dialog moves it from 0xB8A7C6CC (No, the default) to
 *     0xB8A7C6CD (Yes); the main menu's items are 0x4B4E388A on, one a step to
 *     the right, and the Alias Manager's (MC_ProfileManager.fng) too.
 *   - A package's objects: the first at +0x4C, the next at +4; a group (type
 *     +0x18 == 5) holds its own from +0x64.  An object's name hash is at +0x10, its flags
 *     at +0x1C, bit 0 set on the objects not drawn (MC_Main's unused
 *     buttons, MC_List's empty rows, the dialog message while the name field
 *     is up), and the script it runs at +0x54, whose name hash is at +0x30:
 *     0x249DB7B7 is bStringHash("HIGHLIGHT"), which the highlighted button or
 *     list row loops (0x7AB5521A "UNHIGHLIGHT" when it loses the highlight).
 *   - A string (type 2) has its language label's hash at +0x60 (the dialog's
 *     message, "Generic Message" 0x1E2640FA, shows a label per dialog) and its
 *     text, UTF-16, at +0x64 (the alias names in MC_List's rows and in the
 *     name field).
 * MC_List.fng moves no +0x8C: its highlighted row is the one running
 * HIGHLIGHT (the ^ form). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define G_FLOW     0x00925e90u      /* GameFlowManager's state */
#define G_CFENG    0x0091cadcu      /* cFEng* */
#define ENG_HEAD   0xe0u            /* FEngine: the active packages' head */
#define ENG_TAIL   0xe4u            /* and tail: the package on top */
#define PKG_NEXT   0x04u
#define PKG_NAME   0x0cu
#define PKG_HASH   0x10u
#define PKG_INPUT  0x28u            /* byte: the package takes input */
#define PKG_OBJS   0x4cu            /* the first object */
#define PKG_FOCUS  0x8cu            /* the highlighted object */
#define OBJ_NEXT   0x04u
#define OBJ_HASH   0x10u
#define OBJ_TYPE   0x18u            /* 2: a string, 5: a group */
#define OBJ_FLAGS  0x1cu            /* bit 0: not drawn */
#define OBJ_SCRIPT 0x54u            /* the script it runs */
#define GRP_OBJS   0x64u            /* a group's first object */
#define STR_LABEL  0x60u            /* a string's language label hash */
#define STR_TEXT   0x64u            /* and its text, UTF-16 */
#define SCR_HASH   0x30u            /* a script's name hash */
#define SCR_HIGHLIGHT 0x249db7b7u   /* bStringHash("HIGHLIGHT") */

static uint32_t rd(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }

uint32_t mw_flow(void){ return rd(G_FLOW); }

/* a package's name, if it reads as one ("MainMenu.fng"), else 0 */
static const char *pkg_name(uint32_t pkg){
    uint32_t p = rd(pkg + PKG_NAME);
    if (!p || !mem_is_committed(p) || !mem_is_committed(p + 63u)) return 0;
    const char *s = (const char *)(g_mem + p); int n = 0;
    while (n < 63 && s[n] >= 0x20 && s[n] < 0x7f) n++;
    if (s[n] != 0 || n < 5 || strncasecmp(s + n - 4, ".fng", 4)) return 0;
    return s;
}
static uint32_t engine(void){ return rd(rd(G_CFENG)); }

const char *mw_top_screen(void){
    uint32_t e = engine();
    return e ? pkg_name(rd(e + ENG_TAIL)) : 0;
}

/* 1 while a package of this name is in the active list (debug_mw.c's movie wait) */
int mw_fe_active(const char *name){
    uint32_t e = engine(); int k = 0;
    for (uint32_t p = e ? rd(e + ENG_HEAD) : 0u; p && k < 32; p = rd(p + PKG_NEXT), k++){
        const char *nm = pkg_name(p);
        if (nm && !strcasecmp(nm, name)) return 1;
    }
    return 0;
}

/* every Present: the packages new in the active list, head to tail */
void autotest_note_screen(const char *nm);
long autotest_frame(void);          /* autotest.c: the test frame number, the [fng] lines' f= */
#define LIST_MAX 32                 /* the walk's bound: the front end has a dozen at most */
static void state_log(uint32_t pkg, int lv);
void mw_fe_frame(void){
    static struct { uint32_t pkg, hash; } seen[LIST_MAX];
    static int nseen;
    uint32_t pk[LIST_MAX], hs[LIST_MAX]; int n = 0;
    uint32_t e = engine();
    for (uint32_t p = e ? rd(e + ENG_HEAD) : 0u; p && n < LIST_MAX; p = rd(p + PKG_NEXT)){
        pk[n] = p; hs[n] = rd(p + PKG_HASH);
        int old = 0;
        for (int i = 0; i < nseen && !old; i++) old = seen[i].pkg == p && seen[i].hash == hs[n];
        const char *nm = old ? 0 : pkg_name(p);
        if (nm) autotest_note_screen(nm);
        n++;
    }
    /* NFSU2_FNG_LOG=2: the whole active list, head to tail, whenever it changes (a package can come back to the
     * list, or move up it, without being new to it) */
    static int lv = -1;
    if (lv < 0){ const char *v = getenv("NFSU2_FNG_LOG"); lv = v ? atoi(v) : 0; }
    int changed = n != nseen;
    for (int i = 0; i < n && !changed; i++) changed = seen[i].pkg != pk[i] || seen[i].hash != hs[i];
    if (lv >= 2 && changed){
        char line[512]; int o = 0;
        for (int i = 0; i < n && o < (int)sizeof line - 72; i++){
            const char *nm = pkg_name(pk[i]);
            o += snprintf(line + o, sizeof line - (size_t)o, " %s", nm ? nm : "?");
        }
        fprintf(stderr, "[fng] f=%ld active:%s\n", autotest_frame(), n ? line : " none");
    }
    for (int i = 0; i < n; i++){ seen[i].pkg = pk[i]; seen[i].hash = hs[i]; }
    nseen = n;
    if (lv >= 2) for (int i = 0; i < n; i++) state_log(pk[i], lv);
}

/* ---- within a package: the scripts' qualified names (game.h fe_match) ---- */

/* the objects of a package that are drawn, at any depth of its groups (one not drawn hides what it holds), until
 * fn says stop (nonzero) */
typedef int (*ObjFn)(uint32_t obj, void *ctx);
static int walk_drawn(uint32_t first, int depth, ObjFn fn, void *ctx, int *budget){
    for (uint32_t o = rd(first); o && --*budget > 0; o = rd(o + OBJ_NEXT)){
        if (rd(o + OBJ_FLAGS) & 1u) continue;
        if (fn(o, ctx)) return 1;
        if (depth < 8 && rd(o + OBJ_TYPE) == 5u && walk_drawn(o + GRP_OBJS, depth + 1, fn, ctx, budget)) return 1;
    }
    return 0;
}
static int drawn(uint32_t pkg, ObjFn fn, void *ctx){ int budget = 4096; return walk_drawn(pkg + PKG_OBJS, 0, fn, ctx, &budget); }

/* a string's text in ASCII ('?' for the rest), at most cap - 1 characters */
static void str_text(uint32_t obj, char *out, int cap){
    uint32_t s = rd(obj + STR_TEXT); int n = 0;
    for (; s && n < cap - 1 && mem_is_committed(s + 1u); s += 2u){
        unsigned c = g_mem[s] | (unsigned)g_mem[s + 1u] << 8;
        if (!c) break;
        out[n++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    out[n] = 0;
}

struct want { char kind; uint32_t v; char text[64]; };
static int str_is(uint32_t o, void *ctx){
    const struct want *w = ctx;
    if (rd(o + OBJ_TYPE) != 2u) return 0;
    if (w->kind == ':') return rd(o + STR_LABEL) == w->v;
    char t[64]; str_text(o, t, sizeof t);
    for (int i = 0; w->text[i]; i++)
        if (!t[i] || (w->text[i] == '_' ? t[i] != ' ' && t[i] != '_' : t[i] != w->text[i])) return 0;
    uint32_t s = rd(o + OBJ_SCRIPT);
    return w->kind == '\'' || (s && rd(s + SCR_HASH) == SCR_HIGHLIGHT);
}

#define QUALS ":@'^"
/* NAME is PACKAGE followed by one or more of
 *   :LABEL   one of its drawn strings shows the language label of that hash (hex): MC_Main.fng's dialogs
 *   @HASH    its highlighted object has that name hash (hex): a dialog's buttons, the menus' items
 *   'TEXT    one of its drawn strings begins with TEXT: an alias name in a list or in the name field
 *   ^TEXT    and that string runs its HIGHLIGHT script: MC_List.fng's highlighted row
 * and the package is the top one that takes input: the keys reach that one (LEFT on a dialog of MC_Main.fng moves
 * its highlight and not the Alias Manager's under it, which takes input too; MC_List.fng sits above MC_Main.fng
 * while it is up, and MC_Main.fng keeps its last dialog's strings drawn under it).  TEXT matches a string's
 * first characters, '_' a space: the log's spelling (NFSU2_FNG_LOG=3).  1 when all of that holds now.
 * -PACKAGE (no qualifier) is 1 once that package is gone: not in the active list, whatever its input (a closing
 * Pause_Main.fng drops its input in the frame after the RETURN that closes it and leaves the list about 30 frames
 * later; HUD_SingleRace.fng and Autosave_Overlay.fng never take input and stay in the list all through free roam,
 * NFSU2_FNG_LOG=2).
 * NAME|NAME...: any of them (a race's end: the winner's InGamePhotoMaster.fng or the loser's PostRace_Results.fng);
 * a bare PACKAGE there is the top package that takes input. */
int mw_fe_match(const char *name){
    const char *bar = strchr(name, '|');
    if (bar){
        char a[256]; size_t m = (size_t)(bar - name);
        if (m >= sizeof a) return 0;
        memcpy(a, name, m); a[m] = 0;
        return mw_fe_match(a) || mw_fe_match(bar + 1);
    }
    int gone = *name == '-';
    name += gone;
    size_t n = strcspn(name, QUALS);
    uint32_t e = engine(), pkg = 0, q = e ? rd(e + ENG_HEAD) : 0u; int k = 0;
    for (; q && k < LIST_MAX && !pkg; q = rd(q + PKG_NEXT), k++){
        const char *nm = pkg_name(q);
        if (nm && strlen(nm) == n && !strncasecmp(nm, name, n)) pkg = q;
    }
    if (gone) return e && !pkg && !q && !name[n];       /* the whole list walked and the package not in it */
    if (!pkg || !(rd(pkg + PKG_INPUT) & 0xffu)) return 0;
    for (uint32_t p = rd(pkg + PKG_NEXT); p && k < LIST_MAX; p = rd(p + PKG_NEXT), k++)
        if (rd(p + PKG_INPUT) & 0xffu) return 0;
    for (const char *q = name + n; *q; ){
        struct want w = { .kind = *q++ };
        size_t len = strcspn(q, QUALS);
        if (!len || len >= sizeof w.text) return 0;
        memcpy(w.text, q, len); w.text[len] = 0; q += len;
        w.v = (uint32_t)strtoul(w.text, 0, 16);
        if (w.kind == '@'){ uint32_t f = rd(pkg + PKG_FOCUS); if (!f || rd(f + OBJ_HASH) != w.v) return 0; }
        else if (!drawn(pkg, str_is, &w)) return 0;
    }
    return 1;
}

/* NFSU2_FNG_LOG=2: a package's input and highlighted object whenever they change ("[fng] f=N MC_Main.fng input 1
 * @b8a7c6cc"); 3 also what its drawn strings show while it takes input (":label'text ...", ^ for ' when the string
 * runs HIGHLIGHT): the names for the scripts' qualified waits */
struct shows { char *b; int o, cap; uint32_t h; };
static int str_shows(uint32_t o, void *ctx){
    struct shows *s = ctx;
    if (rd(o + OBJ_TYPE) != 2u || s->o >= s->cap - 64) return 0;
    char t[40]; str_text(o, t, sizeof t);       /* a dialog's question to its 39th character */
    for (char *c = t; *c; c++) if (*c == ' ') *c = '_';
    uint32_t sc = rd(o + OBJ_SCRIPT);
    s->o += snprintf(s->b + s->o, (size_t)(s->cap - s->o), " :%08x%s%s", rd(o + STR_LABEL),
                     sc && rd(sc + SCR_HASH) == SCR_HIGHLIGHT ? "^" : "'", t);
    return 0;
}
static void state_log(uint32_t pkg, int lv){
    static struct { uint32_t pkg, input, focus, shows; } st[64];
    static int next;
    int i = 0;
    while (i < 64 && st[i].pkg != pkg) i++;
    if (i == 64){ i = next; next = (next + 1) % 64; st[i].pkg = pkg; st[i].input = st[i].focus = st[i].shows = ~0u; }
    const char *nm = pkg_name(pkg);
    uint32_t input = rd(pkg + PKG_INPUT) & 0xffu, f = rd(pkg + PKG_FOCUS), focus = f ? rd(f + OBJ_HASH) : 0u;
    if (input != st[i].input || focus != st[i].focus)
        fprintf(stderr, "[fng] f=%ld %s input %u @%08x\n", autotest_frame(), nm ? nm : "?", (unsigned)input, focus);
    st[i].input = input; st[i].focus = focus;
    if (lv < 3 || !input) return;
    char b[2048]; struct shows s = { b, 0, (int)sizeof b, 2166136261u };   /* Track_Select.fng draws ~40 strings */
    b[0] = 0; drawn(pkg, str_shows, &s);
    for (const char *c = b; *c; c++) s.h = (s.h ^ (uint8_t)*c) * 16777619u;
    if (s.h != st[i].shows) fprintf(stderr, "[fng] f=%ld %s shows%s\n", autotest_frame(), nm ? nm : "?", b);
    st[i].shows = s.h;
}

/* the object of name hash `hash` in the active package `package`, depth first as the game walks them (groups
 * nest 8 deep at most here, 4096 objects in all), 0: no such package or object */
static uint32_t obj_in(uint32_t first, uint32_t hash, int depth, int *budget){
    for (uint32_t o = rd(first); o && --*budget > 0; o = rd(o + OBJ_NEXT)){
        if (rd(o + OBJ_HASH) == hash) return o;
        if (depth < 8 && rd(o + OBJ_TYPE) == 5u){ uint32_t f = obj_in(o + GRP_OBJS, hash, depth + 1, budget); if (f) return f; }
    }
    return 0;
}
uint32_t mw_fe_object(const char *package, uint32_t hash){
    uint32_t e = engine(); int n = 0;
    for (uint32_t p = e ? rd(e + ENG_HEAD) : 0u; p && n < LIST_MAX; p = rd(p + PKG_NEXT), n++){
        const char *nm = pkg_name(p);
        if (!nm || strcasecmp(nm, package)) continue;
        int budget = 4096;
        return obj_in(p + PKG_OBJS, hash, 0, &budget);
    }
    return 0;
}
