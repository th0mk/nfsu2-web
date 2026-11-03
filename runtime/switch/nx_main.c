/* nx_main.c -- the Nintendo Switch homebrew entry (.nro).  Benchmark build: the game runs headless (no picture, sound
 * or pad: runtime/web/null_platform.c stands in for them) from the SD card, scripted into a race, and logs its frame
 * rate and the guest thread's time per frame to sdmc:/switch/nfsu2/log.txt.  The screen only shows what it is doing.
 *
 * On the SD card:
 *   switch/nfsu2/nfsu2.nro
 *   switch/nfsu2/game/            the game folder (SPEED2.EXE, server.dll, FRONTEND, ...)
 *   switch/nfsu2/race_long.txt    the input script (tests/scripts/race_long.txt)
 * Start it with the title takeover (hold R on a game's icon): it needs ~700 MB, the album's applet mode has 403 MB.
 * This file is compiled with libnx's headers; the runtime is not (their types clash). */
#include <switch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#define DIR "sdmc:/switch/nfsu2"
#ifndef NX_BUILD
#define NX_BUILD 0
#endif
#define NEED_MB 900

int nfsu2_game_main(const char *exe);

/* a data abort or the like: write what the console knows (the fault address register above all) to the log before the
 * process ends; Atmosphere still writes its crash report after this returns */
void __libnx_exception_handler(ThreadExceptionDump *ctx){
    /* no stdio, no malloc: a static buffer and the file system service, appending to crash.txt */
    static char buf[640];
    int n = snprintf(buf, sizeof buf, "b%d exception: type %u, pc %llx, lr %llx, sp %llx, fp %llx, fault address %llx, esr %x, x0 %llx x1 %llx x2 %llx x3 %llx\n",
                     NX_BUILD, (unsigned)ctx->error_desc, (unsigned long long)ctx->pc.x, (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x,
                     (unsigned long long)ctx->fp.x, (unsigned long long)ctx->far.x, (unsigned)ctx->esr, (unsigned long long)ctx->cpu_gprs[0].x,
                     (unsigned long long)ctx->cpu_gprs[1].x, (unsigned long long)ctx->cpu_gprs[2].x, (unsigned long long)ctx->cpu_gprs[3].x);
    if (n <= 0) return;
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return;
    FsFile f; s64 size = 0;
    if (R_FAILED(fsFsOpenFile(fs, "/switch/nfsu2/crash.txt", FsOpenMode_Write | FsOpenMode_Append, &f))){
        fsFsCreateFile(fs, "/switch/nfsu2/crash.txt", 0, 0);
        if (R_FAILED(fsFsOpenFile(fs, "/switch/nfsu2/crash.txt", FsOpenMode_Write | FsOpenMode_Append, &f))) return;
    }
    fsFileGetSize(&f, &size);
    fsFileWrite(&f, size, buf, (u64)n, FsWriteOption_Flush);
    fsFileClose(&f);
}
long nfsu2_status_frames(void);
const uint32_t *gpu_native_shot(unsigned *w, unsigned *h, unsigned *seq);
void gpu_native_start(void);
void audio_nx_start(void);
void lan_nx_start(void);
void lan_nx_stop(void);
void nfsu2_rumble_init(void);
void nfsu2_rumble_tick(void);
void nfsu2_rumble_stop(void);
void audio_nx_stop(void);
void lifecycle_request_quit(const char *why);
void lifecycle_pump(void);
int vfs_dat_mount(const char *path, char *err, size_t errsz);
int vfs_dat_mount_romfs(char *err, size_t errsz);
int nfsu2_text_take(char *init, unsigned cap, unsigned *max, unsigned *kind);
void nfsu2_text_give(const char *utf8);
void gpu_native_stop(void);
unsigned nfsu2_status_flow(void);
const char *nfsu2_status_screen(void);

/* the CPU clock in MHz (clkrst; 0 when it cannot be read): the run-to-run differences in the first benchmark could not
 * be explained without it */
/* the GPU and memory clocks next to it: the boost may have lowered them (the shared power budget) */
static void other_mhz(int *gpu, int *emc){
    static int ok = -1; static ClkrstSession g, e;
    *gpu = *emc = 0;
    if (ok < 0) ok = R_SUCCEEDED(clkrstInitialize()) && R_SUCCEEDED(clkrstOpenSession(&g, PcvModuleId_GPU, 3)) && R_SUCCEEDED(clkrstOpenSession(&e, PcvModuleId_EMC, 3));
    u32 hz = 0;
    if (ok && R_SUCCEEDED(clkrstGetClockRate(&g, &hz))) *gpu = (int)(hz / 1000000u);
    if (ok && R_SUCCEEDED(clkrstGetClockRate(&e, &hz))) *emc = (int)(hz / 1000000u);
}
static int cpu_mhz(void){
    static int ok = -1; static ClkrstSession cs;
    if (ok < 0) ok = R_SUCCEEDED(clkrstInitialize()) && R_SUCCEEDED(clkrstOpenSession(&cs, PcvModuleId_CpuBus, 3));
    u32 hz = 0;
    if (ok && R_SUCCEEDED(clkrstGetClockRate(&cs, &hz))) return (int)(hz / 1000000u);
    return 0;
}

/* GPU overclock: the file /switch/nfsu2/gpuclock.txt holds one number, the GPU clock in MHz (e.g. 768).  Absent or 0: the system's
 * own clock.  Clamped to 307..1075 MHz.  The system hands the clock back on a mode change (docked/handheld, sleep), so it is asked
 * again every few seconds, like cpu_boost; the original rate is put back when the app ends. */
static int g_gpu_want_mhz = -1;
static u32 g_gpu_orig_hz;
static ClkrstSession g_gpu_cs; static int g_gpu_ok;
static void gpu_clock_apply(void){
    if (g_gpu_want_mhz < 0){
        g_gpu_want_mhz = 0;
        FILE *f = fopen(DIR "/gpuclock.txt", "r");
        if (f){ int v = 0; if (fscanf(f, "%d", &v) == 1 && v > 0) g_gpu_want_mhz = v < 307 ? 307 : v > 1075 ? 1075 : v; fclose(f); }
        if (g_gpu_want_mhz){
            g_gpu_ok = R_SUCCEEDED(clkrstInitialize()) && R_SUCCEEDED(clkrstOpenSession(&g_gpu_cs, PcvModuleId_GPU, 3));
            if (g_gpu_ok) clkrstGetClockRate(&g_gpu_cs, &g_gpu_orig_hz);
            fprintf(stderr, "[nx] gpu clock: want %d MHz (system %u MHz), session %s\n", g_gpu_want_mhz, g_gpu_orig_hz / 1000000u, g_gpu_ok ? "ok" : "FAILED");
        }
    }
    if (!g_gpu_want_mhz || !g_gpu_ok) return;
    u32 hz = 0; clkrstGetClockRate(&g_gpu_cs, &hz);
    u32 want_hz = (u32)g_gpu_want_mhz * 1000000u;
    if (hz < want_hz - want_hz / 100 || hz > want_hz + want_hz / 100){
        Result rc = clkrstSetClockRate(&g_gpu_cs, (u32)g_gpu_want_mhz * 1000000u);
        static int n; if (n++ < 5) fprintf(stderr, "[nx] gpu clock set to %d MHz (was %u MHz): result 0x%x\n", g_gpu_want_mhz, hz / 1000000u, (unsigned)rc);
    }
}
static void gpu_clock_restore(void){
    if (g_gpu_want_mhz > 0 && g_gpu_ok && g_gpu_orig_hz) clkrstSetClockRate(&g_gpu_cs, g_gpu_orig_hz);
}

/* flags were renamed to English; an old Portuguese name on the SD card is renamed to the new one once, at start */
static void migrate_flag_names(void){
    static const char *const m[][2] = {
        {"captura","capture"},{"lento.txt","slowdown.txt"},{"resolucao.txt","resolution.txt"},{"semroam","noroam"},{"carros.txt","cars.txt"},
        {"equipe.txt","crew.txt"},{"colisao","collide"},{"travar","lockgfx"},{"restaurar","resetgfx"},{"semvibracao","norumble"},
        {"endereco.txt","address.txt"},{"servidor.txt","server.txt"},{"nome.txt","name.txt"},{"segurar","holdturn"},{"relatorio","report"},{"relatorio.txt","report.txt"},
    };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++){
        char a[96], b[96]; struct stat st;
        snprintf(a, sizeof a, DIR "/%s", m[i][0]); snprintf(b, sizeof b, DIR "/%s", m[i][1]);
        if (stat(a, &st) == 0 && stat(b, &st) != 0) rename(a, b);
    }
}

/* the CPU at its top clock (1785 MHz; a game runs at 1020 unless asked): the race was limited by the game thread, one core
 * at 100%.  The system gives the clock back on a mode change (docked/handheld, sleep), so it is asked again every few seconds.
 * Off unless an empty file /switch/nfsu2/boost exists: with it on (b17-b21) the race ran at 16-21 fps against b16's 38-44. */
static void cpu_boost(void){
    static int ok = -1, noboost = -1; static ClkrstSession cs; static int logged;
    if (noboost < 0){ struct stat st; noboost = stat(DIR "/boost", &st) != 0; }
    if (noboost) return;
    if (ok < 0) ok = R_SUCCEEDED(clkrstInitialize()) && R_SUCCEEDED(clkrstOpenSession(&cs, PcvModuleId_CpuBus, 3));
    if (!ok) return;
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    Result rc = clkrstSetClockRate(&cs, 1785000000u);
    if (!logged){ logged = 1; fprintf(stderr, "[nx] cpu boost to 1785 MHz: result 0x%x\n", (unsigned)rc); }
}

#define PadState GamePadState          /* gamepad.h names its pad like libnx does */
#include "../gamepad.h"
#undef PadState
void nfsu2_pad_state(uint32_t buttons, const float *axis);

/* The console's controller as the game's pad (nx_host.c: gamepad_read; the layout is dinput_pad.c's).  By the label printed on
 * the button, the way a Switch player expects: A selects and B goes back in the menus; in a race A shifts up, B looks back, X
 * shifts down, Y changes the camera, L is nitrous, R the handbrake, ZR the throttle and ZL the brake (the console has no
 * analog triggers: all or nothing), the left stick steers, the D-pad and the sticks' clicks as the game's pad has them,
 * Minus the quit prompt and the reset, Plus the pause.  Minus and Plus held together (a second) leave the game. */
void glr_overlay_toggle(void);
static int pad_to_game(PadState *pad){
    static const struct { u64 btn; unsigned pad; } map[] = {
        { HidNpadButton_A, PAD_A }, { HidNpadButton_B, PAD_B }, { HidNpadButton_X, PAD_X }, { HidNpadButton_Y, PAD_Y },
        { HidNpadButton_L, PAD_LB }, { HidNpadButton_R, PAD_RB }, { HidNpadButton_Minus, PAD_BACK }, { HidNpadButton_Plus, PAD_START },
        { HidNpadButton_StickL, PAD_LS }, { HidNpadButton_StickR, PAD_RS },
        { HidNpadButton_Up, PAD_DUP }, { HidNpadButton_Down, PAD_DDOWN }, { HidNpadButton_Left, PAD_DLEFT }, { HidNpadButton_Right, PAD_DRIGHT },
    };
    static int exit_frames;
    u64 now = padGetButtons(pad);
    { /* the file "padlog": every button press/release goes to the log with the time, the frame and the screen on top, so a hand-played run
       * (yuzu with a real controller) can be turned into a script and its waits measured */
        static int on = -1; static u64 was; struct stat st_;
        if (on < 0) on = stat(DIR "/padlog", &st_) == 0;
        if (on && now != was){
            u64 ch = now ^ was;
            static struct timespec t0_; if (!t0_.tv_sec) clock_gettime(CLOCK_MONOTONIC, &t0_);
            struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
            double sec = (double)(tn.tv_sec - t0_.tv_sec) + (double)(tn.tv_nsec - t0_.tv_nsec) / 1e9;
            fprintf(stderr, "[pad] t=%.2fs frame=%ld screen=%s buttons 0x%llx changed 0x%llx (%s)\n", sec, nfsu2_status_frames(), nfsu2_status_screen(),
                    (unsigned long long)now, (unsigned long long)ch, (now & ch) ? "pressed" : "released");
        }
        was = now;
    }
    int both = (now & HidNpadButton_Minus) && (now & HidNpadButton_Plus);
    exit_frames = both ? exit_frames + 1 : 0;
    { static int both_sticks_was; int bs = (now & HidNpadButton_StickL) && (now & HidNpadButton_StickR);       /* L3 + R3: the fps counter */
      if (bs && !both_sticks_was) glr_overlay_toggle();
      both_sticks_was = bs; }
    uint32_t buttons = 0;
    if (!both) for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (now & map[i].btn) buttons |= 1u << map[i].pad;
    HidAnalogStickState l = padGetStickPos(pad, 0), r = padGetStickPos(pad, 1);
    float axis[PAD_NAXES] = { l.x / 32767.0f, -l.y / 32767.0f, r.x / 32767.0f, -r.y / 32767.0f,
                              (now & HidNpadButton_ZL) ? 1.0f : 0.0f, (now & HidNpadButton_ZR) ? 1.0f : 0.0f };
    nfsu2_pad_state(buttons, axis);
    return exit_frames >= 60;
}

/* The graphics the console ships with: medium, at 720p at most.  The game's own highest settings (app/registry-defaults.txt) with
 * the costly ones off: car and road reflections, motion blur, depth of field, overbright and bleach bypass (on a phone the same
 * took a race from 1,639 draws a frame to 1,118 and the GPU time down by half), and the light streaks, the colour tinting and the
 * crowd (measured on the PC over the same Quick Race, render passes a frame: the game's low defaults 1, this preset 2, the first
 * version of it 4: the streaks cost two, fog and rain one, the tinting/crowd/particles/animated textures one; shadows, the level of
 * detail and g_PerformanceLevel nothing).  Shadows, fog, rain, particles, light glow and the world detail stay.  The row is the runtime's "720p" (com.c modes_refresh), and NFSRES pins the real size at 1280x720 whatever
 * row the Options screen picks.  Written into the registry file at every start; the game's own writes of g_ values are dropped
 * (win32_ext.c, NFSU2_LOCK_GFX).  An empty file /switch/nfsu2/unlock leaves the game's options alone. */
static const struct { const char *name; unsigned v; } k_preset[] = {
    { "g_CarEnvironmentMapEnable", 0 }, { "g_RoadReflectionEnable", 0 }, { "g_MotionBlurEnable", 0 },
    { "g_DepthOfFieldEnable", 0 }, { "g_OverBrightEnable", 0 }, { "g_BleachByPassEnable", 0 },
    { "g_CarShadowEnable", 2 }, { "g_CarHeadlightEnable", 1 }, { "g_FogEnable", 1 }, { "g_LightStreaksEnable", 0 },
    { "g_LightGlowEnable", 1 }, { "g_AnimatedTextureEnable", 1 }, { "g_ParticleSystemEnable", 1 }, { "g_WorldLodLevel", 3 },
    { "g_CarLodLevel", 1 }, { "g_TintingEnable", 0 }, { "g_HorizonFogEnable", 1 }, { "g_RainEnable", 1 },
    { "g_TextureFiltering", 2 }, { "g_FSAALevel", 4294967295u }, { "g_RacingResolution", 0 }, { "g_PerformanceLevel", 5 },
    { "g_VSyncOn", 0 }, { "g_CrowdEnable", 0 }, { "g_CarDamageEnable", 0 }, { "g_CarLightingEnable", 0 },
    { "g_CarEnvironmentMapUpdateData", 1 },
    { "g_SwitchPresetV", 1 },                    /* marks the registry as set up: the preset is written once, then the options are the player's */
};
static void apply_graphics_preset(const char *path){
    enum { MAXL = 512 };
    static char lines[MAXL][160]; int n = 0;
    FILE *f = fopen(path, "r");
    if (f){ while (n < MAXL && fgets(lines[n], sizeof lines[0], f)) n++; fclose(f); }
    int again = access(DIR "/resetgfx", F_OK) == 0;                 /* an empty file "resetgfx": the preset again, once */
    for (int i = 0; i < n && !again; i++)
        if (lines[i][0] == 'd' && lines[i][1] == ' ' && !strncasecmp(lines[i] + 2, "g_SwitchPresetV=", 16)){ fprintf(stderr, "[nx] graphics: the player's options (the preset was written before)\n"); return; }
    if (again) unlink(DIR "/resetgfx");
    for (unsigned k = 0; k < sizeof k_preset / sizeof k_preset[0]; k++){
        size_t nl = strlen(k_preset[k].name); int hit = -1;
        for (int i = 0; i < n; i++)
            if (lines[i][0] == 'd' && lines[i][1] == ' ' && !strncasecmp(lines[i] + 2, k_preset[k].name, nl) && lines[i][2 + nl] == '=') { hit = i; break; }
        if (hit < 0){ if (n >= MAXL) continue; hit = n++; }
        snprintf(lines[hit], sizeof lines[0], "d %s=%u\n", k_preset[k].name, k_preset[k].v);
    }
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f){ fprintf(stderr, "[nx] graphics preset: cannot write %s\n", tmp); return; }
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    if (fclose(f) == 0 && (unlink(path), rename(tmp, path)) == 0)       /* FAT: no rename over an existing file */ fprintf(stderr, "[nx] graphics: Switch preset (medium, 720p), %d registry lines\n", n);
    else fprintf(stderr, "[nx] graphics preset: could not replace %s\n", path);
}

static volatile int g_done, g_rc;

/* report.txt: what one run says about itself, for whoever reads it afterwards (written when the app leaves) */
static struct { unsigned secs; double sum, min; } g_rep[8];
static unsigned g_rep_flow[8];
int nfsu2_roam_friends(void);
static struct { unsigned secs; double sum, min; } g_repf[6];      /* the race (flow 6) by the friends established: 0, 1, 2, 3, 4, 5 or more */
static void report_sample(unsigned flow, double fps){
    if (flow > 7 || fps <= 0.0) return;
    if (flow == 6){
        int n = nfsu2_roam_friends(); if (n < 0) n = 0; if (n > 5) n = 5;
        g_repf[n].secs++; g_repf[n].sum += fps;
        if (g_repf[n].secs == 1 || fps < g_repf[n].min) g_repf[n].min = fps;
    }
    g_rep[flow].secs++; g_rep[flow].sum += fps;
    if (g_rep[flow].secs == 1 || fps < g_rep[flow].min) g_rep[flow].min = fps;
}
static void report_write(const char *how, unsigned seconds){
    FILE *f = fopen(DIR "/report.txt", "w");
    if (!f) return;
    extern int vfs_dat_mounted(void);
    fprintf(f, "NFSU2 b%d (%s), run of %u s, ended: %s\n", NX_BUILD, __DATE__, seconds, how);
    fprintf(f, "game data: %s\n", vfs_dat_mounted() ? "nfsu2.dat" : "the game/ folder");
    static const char *names[8] = { "start", "-", "-", "menus", "loading", "loading", "race", "-" };
    for (unsigned i = 0; i < 8; i++)
        if (g_rep[i].secs) fprintf(f, "fps in %s (flow %u): %.1f average, %.1f lowest, %u s\n", names[i], i, g_rep[i].sum / g_rep[i].secs, g_rep[i].min, g_rep[i].secs);
    for (int n = 0; n < 6; n++)
        if (g_repf[n].secs) fprintf(f, "fps in the race with %d%s friends: %.1f average, %.1f lowest, %u s\n", n, n == 5 ? " or more" : "", g_repf[n].sum / g_repf[n].secs, g_repf[n].min, g_repf[n].secs);
    extern void nfsu2_report_extra(FILE *f);
    nfsu2_report_extra(f);
    fclose(f);
}

void nfsu2_pin(unsigned mask);
static void *game_thread(void *unused){
    (void)unused;
    nfsu2_pin(0x4);                          /* the game's main thread has a core of its own */
    gpu_native_start();                      /* the renderer drains what the recorder writes */
    audio_nx_start();                        /* and the sound mixer's ring goes to audout */
    lan_nx_start();                          /* the network, only with the file /switch/nfsu2/online */
    g_rc = nfsu2_game_main(DIR "/game/SPEED2.EXE");
    fprintf(stderr, "[nx] game ended: %d\n", g_rc);
    fflush(stderr);
    g_done = 1;
    return NULL;
}

static int wait_for_button(PadState *pad, const char *msg){
    printf("%s\n\nPress + to exit.\n", msg);
    consoleUpdate(NULL);
    while (appletMainLoop()){
        padUpdate(pad);
        if (padGetButtonsDown(pad) & (HidNpadButton_Plus | HidNpadButton_B)) break;
        svcSleepThread(50000000ll);
    }
    return 0;
}

/* Who plays online: the console's user (the one that opened the game) gives the name and, hashed, the id the server keeps the
 * ranking and the name's four digits by.  Neither depends on the game's own profile, so a new profile does not start a new
 * ranking.  /switch/nfsu2/name.txt (one line) overrides the name. */
static void online_identity(void){
    char name[0x21] = "", idhex[33] = "";
    AccountUid uid = {{0, 0}};
    if (R_SUCCEEDED(accountInitialize(AccountServiceType_Application))){
        if (R_FAILED(accountGetPreselectedUser(&uid)) || !accountUidIsValid(&uid)){
            s32 n = 0; AccountUid all[ACC_USER_LIST_SIZE];
            if (R_SUCCEEDED(accountListAllUsers(all, ACC_USER_LIST_SIZE, &n)) && n > 0) uid = all[0];
        }
        AccountProfile pr; AccountProfileBase base; AccountUserData ud;
        if (accountUidIsValid(&uid) && R_SUCCEEDED(accountGetProfile(&pr, uid))){
            if (R_SUCCEEDED(accountProfileGet(&pr, &ud, &base))) snprintf(name, sizeof name, "%s", base.nickname);
            accountProfileClose(&pr);
        }
        accountExit();
    }
    if (accountUidIsValid(&uid)){
        char seed[48]; u8 h[32];
        memcpy(seed, "nfsu2-switch-user", 17); memcpy(seed + 17, uid.uid, 16);
        sha256CalculateHash(h, seed, 33);
        for (int i = 0; i < 16; i++) snprintf(idhex + 2 * i, 3, "%02x", h[i]);
        setenv("NFSU2_NX_ID", idhex, 1);
    }
    int manual = 0;
    FILE *f = fopen(DIR "/name.txt", "r");
    if (f){ char l[64]; if (fgets(l, sizeof l, f)){ l[strcspn(l, "\r\n")] = 0; if (l[0]){ snprintf(name, sizeof name, "%s", l); manual = 1; } } fclose(f); }
    char out[16]; int n = 0, gap = 0;                          /* the Online form: letters, digits, - and _; the rest one _ */
    for (const unsigned char *p = (const unsigned char *)name; *p && n < 15; p++){
        if (isalnum(*p) || *p == '-' || *p == '_'){ if (gap && n) out[n++] = '_'; gap = 0; if (n < 15) out[n++] = (char)*p; }
        else gap = 1;
    }
    out[n] = 0;
    int alnum = 0; for (int i = 0; i < n; i++) if (isalnum((unsigned char)out[i])) alnum = 1;
    if (!alnum) snprintf(out, sizeof out, "Piloto");           /* a nickname with no letter the game's alphabet has */
    /* The server keeps claimed names (a keyphrase's, set on the site): a player without that keyphrase who plays under one is an
     * "impostor" and is neither listed to nor shown to anyone in free roam (hub.rs).  This console has no keyphrase, so it plays
     * under the name with four digits in place of its end, the same ones for this user and name (web/src/app/claim.ts digitsFor),
     * unless name.txt names it exactly. */
    if (!manual && idhex[0]){
        char sd[96]; u8 h[32]; int k = snprintf(sd, sizeof sd, "%s\n", idhex);
        for (int i = 0; out[i] && k < (int)sizeof sd - 1; i++) sd[k++] = (char)tolower((unsigned char)out[i]);
        sha256CalculateHash(h, sd, k);
        unsigned dg = (((unsigned)h[0] << 24 | (unsigned)h[1] << 16 | (unsigned)h[2] << 8 | h[3]) % 9000u) + 1000u;
        char cut[16]; snprintf(cut, sizeof cut, "%.11s%u", out, dg); snprintf(out, sizeof out, "%s", cut);
    }
    setenv("NFSU2_ROAM_NAME", out, 1);
    fprintf(stderr, "[nx] online identity: name %s, id %s\n", out, idhex[0] ? "from the console user" : "none (id.txt)");
}

void touch_nx_init(void); void touch_nx_poll(void); void touch_nx_report(FILE *f);

/* the last three runs' log.txt and report.txt are kept: log.txt (this run), log1.txt (the one before), log2.txt, likewise report (FAT: no
 * rename over a file, so the target goes first) */
void nfsu2_remove_tree(const char *path);          /* fsutil_nx.c (this file's DIR is a folder macro, not the type) */

static void rotate_logs(void){
    static const char *base[2] = { DIR "/log", DIR "/report" };
    for (int b = 0; b < 2; b++){
        char a[96], c[96];
        snprintf(a, sizeof a, "%s2.txt", base[b]); unlink(a);
        snprintf(a, sizeof a, "%s1.txt", base[b]); snprintf(c, sizeof c, "%s2.txt", base[b]); rename(a, c);
        snprintf(a, sizeof a, "%s.txt", base[b]); snprintf(c, sizeof c, "%s1.txt", base[b]); rename(a, c);
    }
}

void capture_nx_poll(void);

int main(int argc, char **argv){
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    touch_nx_init();
    PadState pad; padInitializeDefault(&pad);
    mkdir(DIR, 0777);
    mkdir(DIR "/save", 0777);
    rotate_logs();
    FILE *lf = freopen(DIR "/log.txt", "w", stderr);
    if (lf) setvbuf(stderr, NULL, _IOLBF, 0);
    nfsu2_rumble_init();                                          /* after the log is open: it says what the controllers answered */

    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    AppletType at = appletGetAppletType();
    fprintf(stderr, "[nx] NFSU2 b%d, built " __DATE__ " " __TIME__ "\n", NX_BUILD);
    fprintf(stderr, "[nx] nfsu2 benchmark: memory %llu MB total, %llu MB used, applet type %d\n",
            (unsigned long long)(total >> 20), (unsigned long long)(used >> 20), (int)at);
    printf("Need for Speed: Underground 2\nmemory: %llu MB (needs %d MB)\n\n", (unsigned long long)(total >> 20), NEED_MB);

    if ((total >> 20) < NEED_MB){
        fprintf(stderr, "[nx] too little memory: this is the applet mode; start with title takeover (hold R)\n");
        wait_for_button(&pad, "NOT ENOUGH MEMORY: this was opened in applet mode\n(from the Album?).\n\n"
                              "Close this, then on the HOME screen hold R\nwhile you press A on a game, and pick nfsu2.");
        consoleExit(NULL);
        return 0;
    }

    struct stat st;
    migrate_flag_names();
    gpu_clock_apply();
    /* the game as one file: nfsu2.dat, when it is there (else the folder game/, as in the test builds) */
    /* the NSP build carries nfsu2.dat in its own RomFS: that one first (an NRO from the homebrew menu has no RomFS and goes on to the SD card) */
    int dat_in_romfs = 0;
    { char err[240]; int rr = vfs_dat_mount_romfs(err, sizeof err);
      if (rr == 0){ dat_in_romfs = 1;
        /* the NSP is installed and opened, nothing else is copied: online is on unless the file "offline" says otherwise */
        struct stat so; if (stat(DIR "/online", &so) != 0 && stat(DIR "/offline", &so) != 0){ FILE *of = fopen(DIR "/online", "w"); if (of) fclose(of); } }
      else if (rr > 1){ fprintf(stderr, "[nx] %s\n", err); wait_for_button(&pad, err); consoleExit(NULL); return 0; } }
    if (!dat_in_romfs && stat(DIR "/nfsu2.dat", &st) == 0){
        char err[240];
        if (vfs_dat_mount(DIR "/nfsu2.dat", err, sizeof err) != 0){
            fprintf(stderr, "[nx] %s\n", err);
            wait_for_button(&pad, err);
            consoleExit(NULL);
            return 0;
        }
    }
    if (stat(DIR "/game/SPEED2.EXE", &st) != 0){
        fprintf(stderr, "[nx] %s/game/SPEED2.EXE not found\n", DIR);
        wait_for_button(&pad, "SPEED2.EXE not found in\nsdmc:/switch/nfsu2/game/");
        consoleExit(NULL);
        return 0;
    }

    /* no socketInitialize: the benchmark does not use the network (and a process that dies with the socket service
     * open took bsdsocket down with it) */
    /* with an input script (show.txt: a benchmark) the game runs on a save of its own that starts empty every time, so the script
     * meets the same screens on every run and the player's profiles are not touched */
    /* ...unless the file "keepsave" is there: the script then runs on the real save folder (a profile that already exists, e.g. copied
     * from the console), so it does not create a profile every time.  "fnglog" logs every change of the top screen (tests read it). */
    if (stat(DIR "/fnglog", &st) == 0) setenv("NFSU2_FNG_LOG", "1", 1);
    if (stat(DIR "/show.txt", &st) == 0 && stat(DIR "/keepsave", &st) != 0){ nfsu2_remove_tree(DIR "/save_bench"); mkdir(DIR "/save_bench", 0777); setenv("NFSU2_SAVEDIR", DIR "/save_bench", 1); fprintf(stderr, "[nx] test: script show.txt, save_bench starts empty\n"); }
    else { setenv("NFSU2_SAVEDIR", DIR "/save", 1); if (stat(DIR "/show.txt", &st) == 0) fprintf(stderr, "[nx] test: script show.txt on the real save folder (keepsave)\n"); }
    setenv("NFSU2_REGFILE", DIR "/.nfsu2_registry", 1);
    /* graphics: the medium preset the first time (and again with the file "resetgfx"); after that the options are the player's.
     * Whatever they pick, the device never goes above 720p (NFSRES pins it: the console cannot carry more).  An empty file "lockgfx"
     * drops the game's own writes of the graphics options again (the old locked console); "unlock" leaves the registry alone. */
    if (stat(DIR "/unlock", &st) != 0) apply_graphics_preset(DIR "/.nfsu2_registry");
    if (stat(DIR "/lockgfx", &st) == 0) setenv("NFSU2_LOCK_GFX", "1", 1);
    { /* the file "resolution.txt" (one line, WxH, e.g. 960x540) draws the game smaller and the console stretches it to the screen;
       * never above 1280x720 */
        char res[24] = "1280x720"; FILE *rf = fopen(DIR "/resolution.txt", "r");
        if (rf){ unsigned w = 0, h = 0; char l[40]; if (fgets(l, sizeof l, rf) && sscanf(l, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 1280 && h <= 720) snprintf(res, sizeof res, "%ux%u", w, h); fclose(rf); }
        setenv("NFSRES", res, 1);
        fprintf(stderr, "[nx] graphics: drawn at %s, shown at 1280x720\n", res);
    }
    setenv("NFSU2_STATUS_EVERY", "120", 1);
    /* test aids (yuzu or a console nobody is looking at): "capture" saves the picture as capture.png every few seconds; "slowdown.txt" (a
     * number K) makes the game thread K times slower, to stand in for the console's CPU on a PC */
    if (stat(DIR "/capture", &st) == 0) setenv("NFSU2_NX_SHOTS", "120", 1);
    { FILE *lf2 = fopen(DIR "/slowdown.txt", "r"); if (lf2){ char l[24]; if (fgets(l, sizeof l, lf2) && atof(l) > 1.0){ l[strcspn(l, "\r\n")] = 0; setenv("NFSU2_SLOWDOWN", l, 1); fprintf(stderr, "[nx] test: game thread slowed %sx\n", l); } fclose(lf2); } }
    /* online (lan_nx.c): with the file /switch/nfsu2/online the game's free roam is on and the network thread starts.  The console
     * carries 16 friends' cars (a phone starts with the same: the game thread is the limit). */
    if (stat(DIR "/online", &st) == 0){
        /* the other players' cars in free roam cost the game thread: an empty file "noroam" turns them off (Online and LAN stay); "cars.txt"
         * (one number, 1 to 48) sets how many are shown, 2 by default.  The crew (the game's teams: its members see each other at any
         * distance, in the crew's colour, and take the shown places first) is "NFSU2 Switch" by default, so the consoles find each other;
         * "crew.txt" (up to 4 names on one line, separated by commas; empty: no crew) replaces it. */
        if (stat(DIR "/noroam", &st) != 0){
            int cars = 2; FILE *cf = fopen(DIR "/cars.txt", "r");
            if (cf){ int v = 0; if (fscanf(cf, "%d", &v) == 1 && v >= 1 && v <= 48) cars = v; fclose(cf); }
            char cs[8]; snprintf(cs, sizeof cs, "%d", cars);
            setenv("NFSU2_ROAM", "1", 1); setenv("NFSU2_ROAM_MAX_CARS", cs, 1);
            char crew[200] = "NFSU2 Switch"; FILE *ef = fopen(DIR "/crew.txt", "r");
            if (ef){ crew[0] = 0; if (fgets(crew, sizeof crew, ef)) crew[strcspn(crew, "\r\n")] = 0; fclose(ef); }
            if (crew[0]) setenv("NFSU2_ROAM_CREW", crew, 1);
            /* Ghost cars (nobody bumps anybody: they have no collision body, which is also less for the game thread) and a lower
             * rate of snapshots (15 a second, not 20: less to take in and smooth) -- the console's game thread is the limit.  An
             * empty file "collide" makes the other players' cars solid again. */
            setenv("NFSU2_ROAM_COLLIDE", stat(DIR "/collide", &st) == 0 ? "1" : "0", 1);
            setenv("NFSU2_ROAM_RATE", "15", 1); setenv("NFSU2_ROAM_DELAY", "120", 1);
            fprintf(stderr, "[nx] free roam: %d cars shown, crew \"%s\"\n", cars, crew);
        } else fprintf(stderr, "[nx] free roam: off (noroam)\n");
        online_identity();
    }
    setenv("NFSU2_AUDIO_TARGET", "2048", 1);               /* the mixer keeps 46 ms in the ring: audout takes it in 21 ms bites */                  /* a [status] line every 120 frames: fps= and gcpu=ms/f */
    if (stat(DIR "/show.txt", &st) == 0) setenv("NFSU2_INPUT_SCRIPT", DIR "/show.txt", 1);   /* optional: a script instead of the pad */
    /* a safety stop after an hour, except online: free roam reads NFSU2_TIMEOUT as "this is a test run" (roam_plat_test_mode) and would not take the network */
    if (stat(DIR "/online", &st) != 0) setenv("NFSU2_TIMEOUT", "3600", 1);
    int use_gl = stat(DIR "/software", &st) != 0;           /* an empty file named "software" in the folder selects the CPU renderer */
    if (use_gl){ setenv("NFSU2_NX_GL", "1", 1); setenv("GPUVS", "1", 1); setenv("NFSU2_SKIP3D", "0", 1); }
    else { setenv("NFSU2_SKIP3D", "1", 1); setenv("GPUVS", "0", 1); }
    if (use_gl && stat(DIR "/tbuf", &st) == 0) setenv("NFSU2_NX_TBUF", "1", 1);          /* an empty file "tbuf": vertex buffer as a buffer texture (yuzu cannot build the SSBO reads) */
    setenv("NFSU2_RENDERER", "webgpu", 1);                   /* record the draws: gpu_native.c drains them into the renderer */
    fprintf(stderr, "[nx] renderer: %s\n", use_gl ? "OpenGL" : "software");

    pthread_t th; pthread_attr_t pa; pthread_attr_init(&pa);
    pthread_attr_setstacksize(&pa, 1u << 20);
    if (pthread_create(&th, &pa, game_thread, NULL) != 0){
        wait_for_button(&pad, "could not start the game thread");
        consoleExit(NULL);
        return 1;
    }

    /* the picture: the console gives way to a 1280x720 framebuffer */
    consoleExit(NULL);
    Framebuffer fb;
    if (!use_gl){ NWindow *win = nwindowGetDefault(); framebufferCreate(&fb, win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 2); framebufferMakeLinear(&fb); }

    time_t t0 = time(NULL);
    long last_f = 0; time_t last_t = t0; double fps = 0; int tick = 0;
    unsigned shown_seq = 0, painted = 0;
    while (appletMainLoop() && !g_done){
        padUpdate(&pad);
        touch_nx_poll();
        nfsu2_rumble_tick();
        if (pad_to_game(&pad)) break;                                /* Minus and Plus held: leave */
        long f = nfsu2_status_frames(); time_t now = time(NULL);
        if (now > last_t){ fps = (double)(f - last_f) / (double)(now - last_t); last_f = f; last_t = now; report_sample(nfsu2_status_flow(), fps); }
        if (tick % 100 == 0){ cpu_boost(); gpu_clock_apply(); }
        if (tick++ % 100 == 0){
            int gm, em; other_mhz(&gm, &em);
            fprintf(stderr, "[nx] t=%ds frame=%ld fps=%.1f cpu=%dMHz gpu=%dMHz emc=%dMHz screen=%s flow=%u\n",
                    (int)(now - t0), f, fps, cpu_mhz(), gm, em, nfsu2_status_screen(), nfsu2_status_flow());
        }

        /* the game wants text (a profile's name): the system keyboard, with the game's text in it */
        { char init[64]; unsigned tmax, tkind;
          if (nfsu2_text_take(init, sizeof init, &tmax, &tkind)){
              if (getenv("NFSU2_INPUT_SCRIPT")){ nfsu2_text_give(NULL); fprintf(stderr, "[nx] test: script mode, no system keyboard (the script types)\n"); }
              else {
              SwkbdConfig kbd; char out[128] = "";
              if (R_SUCCEEDED(swkbdCreate(&kbd, 0))){
                  swkbdConfigMakePresetDefault(&kbd);
                  swkbdConfigSetHeaderText(&kbd, tkind == 4 ? "E-mail" : "Nome");
                  swkbdConfigSetInitialText(&kbd, init);
                  swkbdConfigSetStringLenMax(&kbd, tmax < 100 ? tmax : 100);
                  Result krc = swkbdShow(&kbd, out, sizeof out);
                  swkbdClose(&kbd);
                  nfsu2_text_give(R_SUCCEEDED(krc) ? out : NULL);
              } else nfsu2_text_give(NULL);
              }
          } }
        if (use_gl){ capture_nx_poll(); svcSleepThread(16000000ll); continue; }     /* the GL renderer presents by itself */
        unsigned w = 0, h = 0, seq = 0;
        const uint32_t *shot = gpu_native_shot(&w, &h, &seq);
        capture_nx_poll();
        /* the framebuffer is double buffered: a new picture is painted into both, then nothing until the next one */
        if (seq != shown_seq){ shown_seq = seq; painted = 0; }
        if (shot && painted >= 2){ svcSleepThread(16000000ll); continue; }
        painted++;
        u32 stride;
        u32 *buf = (u32 *)framebufferBegin(&fb, &stride);
        if (shot && w && h){
            /* the game's picture, scaled to the screen (nearest), ARGB -> the framebuffer's R,G,B,A bytes */
            static unsigned short xmap[1280]; static unsigned xmap_w;
            if (xmap_w != w){ for (unsigned x = 0; x < 1280; x++) xmap[x] = (unsigned short)(x * w / 1280); xmap_w = w; }
            for (unsigned y = 0; y < 720; y++){
                const uint32_t *src = shot + (size_t)(y * h / 720) * w;
                u32 *dst = (u32 *)((u8 *)buf + (size_t)y * stride);
                for (unsigned x = 0; x < 1280; x++){
                    uint32_t p = src[xmap[x]];
                    dst[x] = 0xFF000000u | (p & 0xFF) << 16 | (p & 0xFF00) | ((p >> 16) & 0xFF);
                }
            }
        } else {
            /* nothing drawn yet: a dark screen and a bar that grows with the game's frames (about 3400 to the menu) */
            unsigned fill = (unsigned)(f > 3400 ? 3400 : f) * 1200u / 3400u;
            for (unsigned y = 0; y < 720; y++){
                u32 *dst = (u32 *)((u8 *)buf + (size_t)y * stride);
                for (unsigned x = 0; x < 1280; x++) dst[x] = (y >= 340 && y < 380 && x >= 40 && x < 40 + 1200) ? (x < 40 + fill ? 0xFF50C878u : 0xFF303030u) : 0xFF101010u;
            }
        }
        framebufferEnd(&fb);
        svcSleepThread(30000000ll);
    }
    (void)shown_seq;
    if (!use_gl) framebufferClose(&fb);
    /* Minus and Plus: the game is asked to close (WM_CLOSE, as a window's close box), and is given time to write what it has open.
     * Its thread ends by itself and main returns the way libnx expects; killing the process under a running game thread
     * ended in an Atmosphere "User Break" (2168-0006) and, with svcExitProcess alone, in an error on the console. */
    int asked = !g_done;
    if (asked){
        lifecycle_request_quit("Minus+Plus");
        for (int i = 0; i < 150 && !g_done; i++){ lifecycle_pump(); svcSleepThread(100000000ll); }      /* up to 15 s */
    }
    report_write(!asked ? "the game ended" : g_done ? "left by the player (the game closed)" : "left by the player (the game did not close)", (unsigned)(time(NULL) - t0));
    nfsu2_rumble_stop();
    lan_nx_stop();
    gpu_clock_restore();
    audio_nx_stop();
    gpu_native_stop();                    /* the renderer leaves its GL context before the window goes away */
    fflush(stderr);
    if (!g_done) svcExitProcess();        /* it would not close: a hard stop */
    if (g_rc != 0){
        consoleInit(NULL);
        consoleClear();
        wait_for_button(&pad, "The game ended with an error.\nSee sdmc:/switch/nfsu2/log.txt");
    }
    consoleExit(NULL);
    return g_rc;
}
