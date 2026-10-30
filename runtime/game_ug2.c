/* Need for Speed: Underground 2 (SPEED2.EXE): the game table the runtime's
 * core uses (runtime/game.h).  The features stay in their own files; this
 * file names them (the installs that run before the entry point, the
 * guest_hook chain) and holds what core used to know of SPEED2.EXE itself:
 * UG2's debugging hook sites, its flow state and top screen, the page's
 * drive, pause and text message state, the text entry, the racers of a test
 * trace, the C runtime's ranges, the LAN server glue and its lifted
 * server.dll, the resolution labels, the Online host and DirtySock's clock
 * (an override).  UG2's debugging switches and test traces are in
 * debug_ug2.c, the frame wait loop's sleep in framerate.c.
 *
 * on_frame stays 0: UG2's main-loop work runs in its feature files' own hook
 * at 0x581475 (roam.c, career.c, mprace.c, drift.c, language.c, sms.c). */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include "module.h"
#include "winsock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

void overrides_install(void); void language_install(void); void sponsors_install(void);
void lobbyclick_install(void); void menusize_install(void); void olfilter_install(void);
void tunegraph_install(void); void regional_install(void); void garage_install(void);
void carshadow_install(void); void hornmenu_install(void); void tonemenu_install(void); void powerunits_install(void);
void ug2_debug_present(CPU *c); void ug2_debug_frame(void); void ug2_debug_sound_unlock(void);   /* debug_ug2.c */
void spinsleep_qpc(CPU *c);                                           /* framerate.c */

static void ug2_install(void){
    overrides_install();
    language_install();     /* NFSU2_LANGUAGE: the language pack, the exe's tables */
    sponsors_install();     /* the sponsor screens' Continue (help bar) */
    lobbyclick_install();   /* the lobby's list ignores a click on a dialog over it */
    menusize_install();     /* the help bar's buttons at the page's menu button size */
    olfilter_install();     /* the Online lobby's Filter, once set, narrows its list */
    tunegraph_install();    /* the tuning graphs' dots follow a press and a drag */
    regional_install();     /* the European game's Corsa and 106 for sale, and AI racers' cars */
    garage_install();       /* the career garage's slots 6-10 and Trade Car */
    carshadow_install();    /* a car's shadow 1.5 cm above the road, not 4: off its wheels */
    hornmenu_install();     /* the horns' volume (registry HornVolume) */
    tonemenu_install();     /* Exposure and White Level (registry) to the renderer */
    powerunits_install();   /* power in bhp or kW (registry PowerUnits), its row's methods */
}

/* ---- instruction hooks: the features' sites, then UG2's debugging ones ---- */
int ws_hook(CPU *, uint32_t); int roam_hook(CPU *, uint32_t); int language_hook(CPU *, uint32_t);
int hornmenu_hook(CPU *, uint32_t); int tonemenu_hook(CPU *, uint32_t); int powerunits_hook(CPU *, uint32_t);
int regional_hook(CPU *, uint32_t);
int garage_hook(CPU *, uint32_t); int sndlead_hook(CPU *, uint32_t); int drift_hook(CPU *, uint32_t);
void itrace_dump(int); void watch_dump(const char *, int); void guest_backtrace(CPU *, int);
extern uint64_t g_gs_fail;                     /* debug.c: printed at exit */
static int g_gs_reported = 0;

static const struct { uint32_t ip; const char *what; } g_hook_names[] = {
    {0x0076115eu,"__security_check_cookie entry"},
    {0x00761167u,"jne -> __report_gsfailure"},
    {0x0076112du,"__report_gsfailure body"},
    {0x00761141u,"call reporter(1,0)"},
    {0x0076c25cu,"reporter function entry"},
    {0x0076c2b9u,"took the 'Buffer overrun' branch"},
    {0x0076c2a0u,"reporter: error type switch"},
};
/* --- recursion hunter: sub_0065ae25 --- */
typedef struct { uint32_t a1,a3,esp,cnt,esi,cntw,idx0,s4,eax,e48,edi,edival,inner; } RecEnt;
#define RECR 32
static RecEnt g_rec[RECR];
static uint32_t g_rec_n = 0, g_rec_esp0 = 0;
static int ug2_hook(CPU *c, uint32_t ip){
    if (ws_hook(c, ip)) return 1;
    if (roam_hook(c, ip)) return 1;
    if (language_hook(c, ip)) return 1;
    if (hornmenu_hook(c, ip)) return 1;
    if (tonemenu_hook(c, ip)) return 1;
    if (powerunits_hook(c, ip)) return 1;
    if (regional_hook(c, ip)) return 1;
    if (garage_hook(c, ip)) return 1;
    if (sndlead_hook(c, ip)) return 1;
    if (drift_hook(c, ip)) return 1;
    if (ip == 0x0065ae25u){
        uint32_t a1=MR32(c->esp+4u), a2=MR32(c->esp+8u), a3=MR32(c->esp+12u);
        if (!g_rec_esp0) g_rec_esp0 = c->esp;
        uint32_t esi_ = MR32(c->ecx+0x2cu) + a1*0x64u;
        uint32_t cnt_ = mem_is_committed(esi_+0x34) ? MR16(esi_+0x34) : 0xFFFFu;
        uint32_t ebx_ = (mem_is_committed(esi_+0x14)&&mem_is_committed(esi_))
                        ? (MR32(esi_+0x14) + MR32(esi_)) : 0;
        uint32_t idx0 = (ebx_ && mem_is_committed(ebx_)) ? MR16(ebx_) : 0xFFFFu;
        uint32_t s4   = mem_is_committed(esi_+4) ? MR32(esi_+4) : 0;
        uint32_t sbase= mem_is_committed(esi_)   ? MR32(esi_)   : 0;
        uint32_t eaxv = idx0*0x64u + s4;
        uint32_t e48  = mem_is_committed(eaxv+0x48) ? MR32(eaxv+0x48) : 0;
        uint32_t edi_ = e48 + sbase;
        uint32_t ediw = mem_is_committed(edi_) ? MR16(edi_) : 0xFFFFu;
        uint32_t pc   = mem_is_committed(eaxv+0xc) ? MR32(eaxv+0xc) : 0;
        uint32_t inner= (pc && mem_is_committed(pc+0x10)) ? MR32(pc+0x10) : 0;
        (void)a2;
        g_rec[g_rec_n % RECR] = (RecEnt){a1,a3 & 0xffffu,c->esp,g_rec_n,esi_,cnt_,idx0,s4,eaxv,e48,edi_,ediw,inner};
        g_rec_n++;
        uint32_t depth = (g_rec_esp0 > c->esp) ? (g_rec_esp0 - c->esp) : 0;
        if (depth > 0x400000u){       /* 4 MB of stack used = runaway recursion */
            fprintf(stderr,"\n*** RUNAWAY RECURSION: %u calls, %u KB of stack used\n",
                    g_rec_n, depth/1024u);
            fprintf(stderr,"    last %d calls (arg1, arg2, arg3, esp):\n", RECR);
            uint32_t start = g_rec_n > RECR ? g_rec_n - RECR : 0;
            for (uint32_t k=start;k<g_rec_n;k++){
                RecEnt *e=&g_rec[k % RECR];
                fprintf(stderr,"      #%-7u a1=%-4u outerCnt=%-3u idx0=%-4u [esi+4]=0x%08x"
                               " eax=0x%08x [eax+0x48]=0x%08x edi=0x%08x [edi]=%-5u innerCnt=%u\n",
                        e->cnt, e->a1, e->cntw, e->idx0, e->s4, e->eax, e->e48,
                        e->edi, e->edival, e->inner);
            }
            itrace_dump(12);
            if (g_watch_addr) watch_dump("at the recursion", 20);
            _exit(5);
        }
        return 1;
    }
    if (ip == 0x0066887eu){
        /* movzx esi, word [edi+4] : type/class key */
        static int n=0;
        if (n<40){
            uint32_t e = c->edi;
            uint32_t cls = mem_is_committed(e+4) ? MR16(e+4) : 0xFFFFu;
            uint32_t t   = mem_is_committed(e)   ? MR16(e)   : 0xFFFFu;
            uint32_t el  = mem_is_committed(e+6) ? MR16(e+6) : 0xFFFFu;
            fprintf(stderr,"  [type %2d] edi=0x%08x  [edi]=%-5u [edi+4]=%-5u [edi+6]=%-5u\n",
                    n, e, t, cls, el);
            n++;
        }
        return 1;
    }
    if (ip == 0x0076115eu){                       /* __security_check_cookie entry */
        uint32_t expect = MR32(0x0081a1d0u);
        if (c->ecx != expect){
            g_gs_fail++;
            if (g_gs_reported < 3){
                g_gs_reported++;
                uint32_t cookie_slot = c->ebp - 4u;
                fprintf(stderr,"\n*** /GS COOKIE FAILURE ***\n");
                fprintf(stderr,"  ecx (stack^ret)   = 0x%08x\n", c->ecx);
                fprintf(stderr,"  expected (global) = 0x%08x\n", expect);
                fprintf(stderr,"  ebp               = 0x%08x\n", c->ebp);
                fprintf(stderr,"  COOKIE SLOT       = 0x%08x  (value 0x%08x)\n",
                        cookie_slot, MR32(cookie_slot));
                fprintf(stderr,"  return address [ebp+4] = 0x%08x\n", MR32(c->ebp + 4u));
                fprintf(stderr,"  slot ^ return          = 0x%08x\n",
                        MR32(cookie_slot) ^ MR32(c->ebp + 4u));
                guest_backtrace(c, 8);
                if (g_watch_addr) watch_dump("cookie check failed", 24);
            }
        }
        return 1;
    }
    return 0;
}

/* ---- the flow state and the front end's top screen ----------------------- */
/* GameFlowManager's state [0x8654a4]: 3 = front end, 6 = racing, the others
 * load or unload (discord.c G_FLOW). */
#define G_FLOW 0x008654a4u
static uint32_t ug2_flow(void){ return mem_is_committed(G_FLOW + 3u) ? MR32(G_FLOW) : 0u; }

/* cFEng::pInstance = [0x8384C4]; GetTopPackage (0x50C8E0) returns
 * [[cFEng+8]+0xE4] (FNGInfo*), whose +0x0C points at the "*.fng" name.
 * The name offset is found by scanning for a pointer to a printable string
 * ending in ".fng" (cached once found; +0x0C in v1.2). */
#define G_FENG 0x008384c4u
static int g_fng_off = -1;
static const char *fng_str(uint32_t p){
    if (!p || !mem_is_committed(p) || !mem_is_committed(p + 63)) return 0;
    const char *s = (const char *)(g_mem + p); int n = 0;
    while (n < 63 && s[n] >= 0x20 && s[n] < 0x7f) n++;
    if (s[n] != 0 || n < 5 || strncasecmp(s + n - 4, ".fng", 4)) return 0;
    return s;
}
static const char *ug2_top_screen(void){
    if (!mem_is_committed(G_FENG)) return 0;
    uint32_t fe = MR32(G_FENG);
    if (!fe || !mem_is_committed(fe + 11)) return 0;
    uint32_t mg = MR32(fe + 8);                       /* GetTopPackage (0x50C8E0) */
    if (!mg || !mem_is_committed(mg + 0xe7)) return 0;
    uint32_t pk = MR32(mg + 0xe4);                    /* FNGInfo*; name ptr at +0x0C */
    if (!pk || !mem_is_committed(pk + 255)) return 0;
    if (g_fng_off >= 0){ const char *s = fng_str(MR32(pk + (uint32_t)g_fng_off)); if (s) return s; }
    for (int o = 0; o < 256; o += 4){
        const char *s = fng_str(MR32(pk + (uint32_t)o));
        if (s){ g_fng_off = o; return s; }
    }
    return 0;
}

/* ---- the page's state (runtime/web/status_web.c) ------------------------- */
/* "drive": what the page's touch controls (web/src/app/touch.ts) need of the
 * game, set at every Present (autotest.c): 1 while the player drives in a
 * race, 2 in free roam, 0 elsewhere, plus 4 with a manual gearbox and 8
 * during a cutscene (the touch controls' level horizon waits).  Driving
 * is GameFlow 6 with a race HUD (HUD_*.fng) the top package; the pause menu
 * (UI_pause.fng), the results, the world's loading screen and the Engage
 * dialog over the HUD are not, nor is a cutscene in free roam (NIS, as
 * runtime/discord.c tells one).  A race's start is one too, the camera's
 * flight round the grid and the countdown until a moment after the start, so
 * a race drives during a cutscene: the player revs on the line and goes at
 * once (a player's report: the controls came seconds after the others left).
 * The gearbox is manual in a drag race (the game asks nothing there and sets
 * the car's gearbox to manual), else as the race's Transmission choice set
 * it: the player's setting 1 (0x4a7b30), kept at player 0 (0x8900ac) +0x20
 * and handed to the car's gearbox (0x407280: 0 and 2 shift by themselves, 1
 * and 3 keep the gear).  The gearbox itself is no good to read: in a drag
 * race the car's controls (+0x30) become the game's lane steering once it
 * rolls. */
#define G_PLAYER0    0x008900acu
#define PL_GEARBOX   0x20u
#define G_RACE_DRAG  0x0089e7d8u      /* race parameters (discord.c G_RACE) + R_DRAG */
long metal_frames(void);
int career_fe_active(const char *name);
int roam_exploring(void);
static int manual_gearbox(int race){
    if (race && MR8(G_RACE_DRAG)) return 1;
    uint32_t p = mem_is_committed(G_PLAYER0 + 3u) ? MR32(G_PLAYER0) : 0u;
    if (!p || !mem_is_committed(p + PL_GEARBOX)) return 0;
    uint8_t g = MR8(p + PL_GEARBOX);
    return g == 1u || g == 3u;
}
static int drive_state(void){
    unsigned flow = ug2_flow();
    const char *top = ug2_top_screen();
    int hud = top && !strncasecmp(top, "HUD_", 4) && strcasecmp(top, "HUD_World_Loading.fng");
    int nis = mem_is_committed(0x827ecbu) && (MR32(0x827e98u) | MR32(0x827eb0u) | MR32(0x827ec8u));
    int engage = career_fe_active("UI_EngageMessageDialog.fng");
    int roam = roam_exploring();
    int v = flow == 6u && hud && !engage && !(roam && nis) ? (roam ? 2 : 1) : 0;
    if (v && manual_gearbox(v == 1)) v |= 4;
    if (v && nis) v |= 8;
    /* a line at each change, for a player's log */
    static unsigned l_flow = ~0u; static int l_hud = -1, l_nis = -1, l_engage = -1, l_v = -1;
    if (flow != l_flow || hud != l_hud || nis != l_nis || engage != l_engage || v != l_v){
        l_flow = flow; l_hud = hud; l_nis = nis; l_engage = engage; l_v = v;
        fprintf(stderr, "[drive] f=%ld flow=%u top=%s nis=%d engage=%d -> %d\n", metal_frames(), flow, top ? top : "-", nis, engage, v);
    }
    return v;
}
/* "rate": the frame rate the game paces to now (framerate.c content_rate:
 * the mode's rate, 30 in a cutscene, at most 120 in a network race; 0 =
 * uncapped), so the page can tell whether the game keeps up with it
 * (web/src/app/rendercheck.ts).  "pause": 1 while the game's pause menu
 * (UI_Pause.fng) is the top package, not its options or a question over it,
 * so the page can point to its own menu under the panel
 * (web/src/app/pausehint.ts).  "sms": the text message of ours (sms.c) the
 * game has in its inbox this game, 0 none, so the page asks for it no more
 * (web/src/app/playtime.ts).  "roam_cars", "roam_friends": friends' cars the
 * game holds and friends connected in free roam (roam_game.c), for the
 * page's copy of the log (web/src/app/lastgame.ts): what a phone held when
 * Safari stopped the page.  The friends take the session lock, so every
 * 30th frame. */
int hfr_rate_now(void);
int sms_delivered(void); void sms_request(int id);
int roam_game_cars(void); int roam_game_friends(void);
/* "engage": the player's car on an event's or a shop's marker (player
 * +0x9AC, what Engage would enter), the game's own outrun offer (the career's
 * outrun state 2, set with the HUD's outrun icon by 0x500D20 and tested by
 * 0x500D80), or one of ours (roam_game_engage: a friend's outrun, a
 * multiplayer-only race spot, a lobby to join or to start); "roam_near":
 * friends' cars within a horn's reach (roam_game_near) */
#define PL_MARKER    0x9acu
#define G_OUTRUN_ST  0x00860a58u      /* Career4230 (0x85EF70) +0x1AE8 */
int roam_game_engage(void); int roam_game_near(void);
static void ug2_status(StatusOut *s){
    s->drive = drive_state();
    s->rate = hfr_rate_now();
    const char *top = ug2_top_screen();
    s->pause = top && !strcasecmp(top, "UI_Pause.fng");
    s->sms = sms_delivered();
    static int n, friends;
    s->roam_cars = roam_game_cars();
    if (n++ % 30 == 0) friends = roam_game_friends();
    s->roam_friends = friends;
    int roam = (s->drive & 3) == 2;
    uint32_t p = roam && mem_is_committed(G_PLAYER0 + 3u) ? MR32(G_PLAYER0) : 0u;
    int marker = p && mem_is_committed(p + PL_MARKER + 3u) && MR32(p + PL_MARKER);
    int outrun = roam && mem_is_committed(G_OUTRUN_ST + 3u) && MR32(G_OUTRUN_ST) == 2u;
    s->engage = roam && (marker || outrun || roam_game_engage());
    s->roam_near = roam ? roam_game_near() : 0;
}

/* ---- the game's text entry, for the page (runtime/web/input_web.c) -------- */
/* The game has one text entry, the object at 0x83A7B0 (set up by 0x4FF3B0;
 * 0x55DBD0 hands it every WM_CHAR while +0x210 and +0x214 are set, 0x551270
 * takes it in): +0x100 the text, +0x200 its length, +0x204 its kind
 * (0x4FF310: letters and digits, kind 4 also @._, kind 5 also space,@.,
 * kinds 0 and 3 also 0xC0..0xFF and the punctuation of 0x7FA3D0), +0x20C the
 * most it takes. */
#define G_TXT 0x0083a7b0u
static uint32_t ug2_text_entry(uint32_t *kind, uint32_t *len, uint8_t *text, uint32_t cap){
    if (!(MR8(G_TXT + 0x210u) && MR32(G_TXT + 0x214u))) return 0;
    uint32_t max = MR32(G_TXT + 0x20cu), n = MR32(G_TXT + 0x200u);
    *kind = MR32(G_TXT + 0x204u);
    if (n > max) n = max;
    *len = n;
    for (uint32_t i = 0; i < n && i < cap; i++) text[i] = MR8(G_TXT + 0x100u + i);
    return max;
}

/* ---- test traces' racers (autotest.c, "trace" and "expect") --------------- */
/* GRaceStatus [0x890118]: racer pointers at +0xBD0, count +0x24; the driver
 * type [[[r]+0x14]+4] is 1 for a human. */
#define G_RACESTATUS 0x00890118u
static uint32_t tr_rd(uint32_t a, int *ok){
    if (a && mem_is_committed(a) && mem_is_committed(a + 3)) return MR32(a);
    *ok = 0; return 0;
}
static uint32_t ug2_racer(int want_human, int slot){
    int ok = 1; uint32_t gs = tr_rd(G_RACESTATUS, &ok);
    if (!ok || !gs) return 0;
    uint32_t n = tr_rd(gs + 0x24u, &ok);
    if (!ok || n > 16) return 0;
    if (!want_human) return (uint32_t)slot < n ? tr_rd(gs + 0xbd0u + 4u * (uint32_t)slot, &ok) : 0;
    for (uint32_t i = 0; i < n; i++){
        ok = 1; uint32_t r = tr_rd(gs + 0xbd0u + 4u * i, &ok);
        uint32_t t = tr_rd(tr_rd(tr_rd(r, &ok) + 0x14u, &ok) + 4u, &ok);
        if (ok && t == 1) return r;
    }
    return 0;
}

/* ---- QPCWHO (win32.c): the clock wrapper's caller ------------------------- */
/* sub_0043bdf0 (the game's time wrapper): sub esp,8; push arg; call.
 * After POP32 [esp]=arg, [esp+4..0xb]=locals, [esp+0xc]=its caller. */
static uint32_t ug2_qpc_caller(CPU *c){ return c->eip == 0x0043bdfeu ? MR32(c->esp + 0xcu) : 0u; }

/* ---- DirtySock's clock ---------------------------------------------------- */
/* NetTick (0x74B730) is SPEED2.EXE's `jmp [GetTickCount]` for all of EA's
 * network code: 60 call sites at 0x73DE1C-0x75BB12 (adverts and their
 * expiry, CommUDP / NetGameLink retransmits, keepalives and timeouts, pings,
 * lobby timers, the lobby challenge code); no game-simulation code calls it.
 * The HLE's GetTickCount is the frame-paced virtual clock, which nearly
 * stops while the game thread does not present (race loading, hitches).  On
 * Windows the network thread keeps real time meanwhile, and a Windows peer
 * expects the Mac to: so NetTick returns real monotonic milliseconds
 * (winsock.c's clock).  NFSU2_NETTICK=virtual restores the old clock. */
void sub_0074b730_orig(CPU *c);
void sub_0074b730(CPU *c){
    static int virt = -1;
    if (virt < 0){ const char *e = getenv("NFSU2_NETTICK"); virt = e && !strcmp(e, "virtual"); }
    if (virt){ sub_0074b730_orig(c); return; }
    POP32(c->eip);
    c->eax = (uint32_t)(ws_now_us() / 1000ull);
}

/* ---- the settings menu's resolution labels (com.c) ------------------------ */
/* The menu text comes from ready-made strings in .rdata (sub_0049b690: row
 * -> "640x480".."1600x1200"); com.c writes its own labels there.  Text slots
 * are 8/8/12/12/12/12 bytes: short labels for the first two rows. */
static const ResLabel ug2_res_labels[6] = {
    {0x0078fa1cu, 8}, {0x0078fa14u, 8}, {0x0078fa08u, 12}, {0x0078f9fcu, 12}, {0x0078f9f0u, 12}, {0x0078f9e4u, 12},
};

/* the movie names' language suffix (fileio.c): this version builds "_fr.vp6"
 * into a movie's path, the folder holds "_en" files */
static const char *const ug2_lang_suffixes[] = { "_en", "", "_dn", "_du", "_sw", 0 };

void GAMEINFO_ENTRY_FN(CPU *c);                /* the entry point (gen/all/gameinfo.h, through cpu.h) */

/* what the registry answers for values the game reads before it has
 * written them (the language comes from runtime/language.c) */
static const RegDefault ug2_reg[] = {
    {"Install Dir", "C:\\NFSU2", 0},
    {"CD Drive", "C:\\NFSU2", 0},
    {"StreamingInstall", 0, 0},
    {"VERSION", "1.2.0.0", 0},
    {"SIZE", 0, 0},
    {0, 0, 0},
};

/* the lifted DLLs (module.c): EA's LAN server, gen/server with prefix srv_
 * (gen.sh; a stub module when the game folder has none) */
MODLINK_DECLARE(srv_);
static const ModLink ug2_modules[] = {
    { MODLINK_TABLES(srv_), .slot_lo = MM_SRV_LO, .slot_hi = MM_SRV_HI,
      .heap_base = MM_SRV_HEAP_BASE, .heap_size = MM_SRV_HEAP_SIZE, .heap_n = MM_SRV_HEAP_N,
      .feature = "LAN hosting", .release = "NFSU2 v1.2" },
};

void photo_input(int cmd, int a, int b);       /* photo.c: photo mode, as the page drives it */
void ug2_pad_rescan(int on); unsigned ug2_pad_trigger_axes(uint32_t dev); float ug2_pad_jolt(void);  /* pad_ug2.c */

const GameOps g_game = {
    .id = "ug2",
    .exe_name = "SPEED2.EXE",
    .vroot = "C:\\NFSU2",
    .module_path = "C:\\\\NFSU2\\\\SPEED2.EXE",   /* with doubled backslashes, as UG2 has always been told */
    .reg_defaults = ug2_reg,
    .reg_file = ".nfsu2_registry",              /* ~/.nfsu2_registry without NFSU2_REGFILE, as always */
    .lang_suffixes = ug2_lang_suffixes,
    .entry = GAMEINFO_ENTRY_FN,
    .entry_va = GAMEINFO_ENTRY,
    .modules = ug2_modules,
    .nmodules = (int)(sizeof ug2_modules / sizeof ug2_modules[0]),
    .install = ug2_install,
    .hook = ug2_hook,
    .on_present = ug2_debug_present,
    .on_test_frame = ug2_debug_frame,
    .status = ug2_status,
    .sms_request = sms_request,                 /* sms.c: the page's IEV_SMS */
    .photo_input = photo_input,                 /* photo.c: the page's IEV_PHOTO */
    .flow = ug2_flow,
    .flow_fe = 3,
    .flow_race = 6,
    .top_screen = ug2_top_screen,
    .racer = ug2_racer,
    .text_entry = ug2_text_entry,
    .crt_lo = 0x0075b000u,                      /* the C runtime's code: 0x75B000-0x772000 */
    .crt_hi = 0x00772000u,
    .crtheap_va = 0x008ba4a4u,
    .active_heap_va = 0x008ba4a8u,
    .on_qpc = spinsleep_qpc,                    /* the frame wait loop sleeps (framerate.c) */
    .qpc_caller = ug2_qpc_caller,
    .lan_obj = 0x0089cf48u,                     /* StartLANServer 0x5FA300 (thiscall on 0x89CF48:
                                                 * LoadLibraryA("server"), GetProcAddress x3,
                                                 * StartServer(name, region)), UnloadServer 0x5F08F0
                                                 * (StopServer, FreeLibrary), [0x890128] IsServerRunning */
    .lan_start = 0x005fa300u,
    .lan_stop = 0x005f08f0u,
    .lan_running = 0x00890128u,
    .res_labels = ug2_res_labels,
    .on_sound_unlock = ug2_debug_sound_unlock,
    .online_host_va = 0x007fbdbcu,              /* SPEED2.EXE v1.2 NA: LobbyConnect's host, 0x588630 */
    .online_host = "pcnfs05.ea.com",
    .pad_rescan = ug2_pad_rescan,               /* pad_ug2.c: the 32-frame rescan's flag [0x8364E8] */
    .pad_trigger_axes = ug2_pad_trigger_axes,   /* what the pad's input channel reads (0x874C40 + n*0x370) */
    .pad_jolt = ug2_pad_jolt,                   /* the player car's hits as rumble */
    .pad_hold_screen = "UI_Options_PC_Controller.fng",  /* the Controls page: a lost pad crashes it */
    .pad_fe_keys = 1,                           /* text fields and the quit prompt take characters only */
};
