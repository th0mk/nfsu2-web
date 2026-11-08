/* features.c -- the runtime's features in one place: what each installs at
 * boot, its instruction hooks, its calls every frame.
 *
 * Three lists, each run in its own order, as their calls depend on it: the
 * installs in the boot's, the hook chain stops at the first hook that takes
 * the address, the frame calls run one after another from the main loop's
 * hook.  A row per feature with all three would not keep them: roam.c's hook
 * comes before language.c's and its frame call after; damage.c's hook is the
 * last, its frame call the first.
 *
 * Every build has the same hooks and frame calls; the installs say which
 * builds run them at boot (FT_WEB: web/build.sh, FT_MAC: link.sh). */
#include "featurelist.h"
#include <stddef.h>

enum { FT_WEB = 1, FT_MAC = 2, FT_ALL = FT_WEB | FT_MAC };
#ifdef __wasm__
#define FT_HERE FT_WEB
#else
#define FT_HERE FT_MAC
#endif
#define COUNT(a) (sizeof (a) / sizeof (a)[0])

/* ---- at boot: after win32_init, before autotest_init --------------------- */
static const struct { const char *name; void (*install)(void); unsigned builds; } k_installs[] = {
    { "overrides",   overrides_install,   FT_ALL },   /* the runtime's own dispatch patches and the D3DX stand-ins */
    { "language",    language_install,    FT_ALL },   /* NFSU2_LANGUAGE: the language pack, the exe's tables */
    { "sponsors",    sponsors_install,    FT_ALL },   /* the sponsor screens' Continue (help bar) */
    { "lobbyclick",  lobbyclick_install,  FT_ALL },   /* the lobby's list ignores a click on a dialog over it */
    { "menusize",    menusize_install,    FT_WEB },   /* the help bar's buttons at the page's menu button size */
    { "olfilter",    olfilter_install,    FT_ALL },   /* the Online lobby's Filter, once set, narrows its list */
    { "tunegraph",   tunegraph_install,   FT_WEB },   /* the tuning graphs' dots follow a press and a drag (touch) */
    { "dyno",        dyno_install,        FT_ALL },   /* the dyno's Save Profile and Quit Tuning keep its setups in the car */
    { "regional",    regional_install,    FT_ALL },   /* the European game's Corsa and 106 for sale, and AI racers' cars */
    { "garage",      garage_install,      FT_ALL },   /* the career garage's slots 6-10 and Trade Car */
    { "vinylpacks",  vinylpacks_install,  FT_ALL },   /* the Japanese release's Art Factory 28-34 (VINYLPACKS) */
    { "specialties", specialties_install, FT_ALL },   /* Car Specialties: Stock gauges again, Trunk Audio's Back */
    { "stance",      stance_install,      FT_ALL },   /* the Body Shop's Stance: its page's messages */
    { "showoff",     showoff_install,     FT_ALL },   /* the animation library's heap: room for show-off's animations, its end checked */
    { "carshadow",   carshadow_install,   FT_ALL },   /* a car's shadow 1.5 cm above the road, not 4: off its wheels */
    /* the next three on the Mac at their first use, from their hooks; tonemenu's pass is the page's only */
    { "hornmenu",    hornmenu_install,    FT_WEB },   /* the horns' volume (registry HornVolume) */
    { "tonemenu",    tonemenu_install,    FT_WEB },   /* Exposure and White Level (registry) to the renderer */
    { "powerunits",  powerunits_install,  FT_WEB },   /* power in bhp or kW (registry PowerUnits), its row's methods */
};

/* ---- instruction hooks: the first that takes the address ends the chain -- */
static const struct { const char *name; int (*hook)(CPU *c, uint32_t ip); } k_hooks[] = {
    { "splitscreen", splitscreen_hook },   /* split screen's views, maps and HUDs */
    { "coop",        coop_hook },          /* co-op: player 2 in the career's races */
    { "widescreen",  ws_hook },            /* the projection on screens wider or narrower than 4:3 */
    { "roam",        roam_hook },          /* the main loop's hook (features_frame), the HUD step, the world's step */
    { "language",    language_hook },      /* subtitles in the language packs */
    { "hornmenu",    hornmenu_hook },      /* Horn Volume in the Audio options */
    { "tonemenu",    tonemenu_hook },      /* Exposure and White Level in the Display options */
    { "powerunits",  powerunits_hook },    /* Power Units in the Gameplay options, and the power's units */
    { "shownames",   shownames_hook },     /* Show Player Names in the LAN and Online options */
    { "hudbank",     hudbank_hook },       /* career free roam's HUD parts only when they say something */
    { "regional",    regional_hook },      /* the AI racers' cars */
    { "garage",      garage_hook },        /* the shop's buy for a car in slots 6-10 */
    { "vinylcat",    vinylcat_hook },      /* the hidden vinyls' category in the vinyl shop */
    { "stance",      stance_hook },        /* Stance in the Body Shop, its page of sliders */
    { "sndlead",     sndlead_hook },       /* the sound's usual delay soon after a load */
    { "drift",       drift_hook },         /* drift mode's and race_music's music */
    { "quickrace",   quickrace_hook },     /* Quick Race's laps and opponents */
    { "trafficcars", trafficcars_hook },   /* a traffic vehicle as the player's car */
    { "travel",      travel_hook },        /* the World Map shut on a race track's map */
    { "damage",      damage_hook },        /* crash damage on the cars' meshes */
};

/* ---- every frame, from the main loop's hook (roam.c) -------------------- */
__attribute__((weak)) void damage_frame(CPU *c);   /* only where damage.c or damage_none.c is linked */
/* the frame calls that take no CPU */
static void f_hiddenraces(CPU *c){ (void)c; hiddenraces_frame(); }
static void f_visualrep(CPU *c){ (void)c; visualrep_frame(); }
static void f_whine(CPU *c){ (void)c; whine_frame(); }
static void f_carspot(CPU *c){ (void)c; carspot_frame(); }
static void f_walls(CPU *c){ (void)c; walls_frame(); }
static void f_savegame(CPU *c){ (void)c; savegame_frame(); }
/* the multiplayer feature: roam_game.c's, or in out/nfsu2_asipeer the Windows plugin under Unicorn (roam_asipeer.c) */
static void f_roam_game(CPU *c){ if (!roam_asipeer_frame(c)) roam_game_frame(c); }

static const struct { const char *name; void (*frame)(CPU *c); } k_frames[] = {
    { "damage",      damage_frame },           /* crash damage's texture pack */
    { "reads",       reads_frame },            /* overrides.c: the streamer's reads and Loading screens, for the log */
    { "intro",       intro_frame },            /* the title screen's line under the copyright */
    { "language",    language_texts_frame },   /* texts of our own in the game's language table */
    { "sms",         sms_frame },              /* a text message of ours on the game's phone */
    { "hiddenraces", f_hiddenraces },          /* the career's hidden races on the map (show_hidden_races) */
    { "visualrep",   f_visualrep },            /* the visual rating by the stage (visual_rep_remover) */
    { "whine",       f_whine },                /* the gearbox whine (gearbox_whine) */
    { "carspot",     f_carspot },              /* the career's kept spot, if no place, the new career's start */
    { "walls",       f_walls },                /* a car past the career's walls back on the roads it can reach */
    { "travel",      travel_frame },           /* Tracks: free roam on a race track's map (F9) and back */
    { "mprace",      mprace_frame },           /* a multiplayer race's trip from free roam and back */
    { "mpcredit",    mpcredit_frame },         /* a multiplayer race's win that waits for the crib */
    { "roam_game",   f_roam_game },            /* the multiplayer feature */
    { "racehorn",    racehorn_frame },         /* horns in races between players */
    { "racenet",     racenet_frame },          /* how the other players' cars came in, at a race's end */
    { "optslider",   optslider_frame },        /* our option sliders' copied parts (the pause menu's Horn Volume) */
    { "stance",      stance_frame },           /* the Stance page's look */
    { "drift",       drift_frame },            /* drift mode in free roam (F2) */
    { "cameras",     cameras_frame },          /* the player's view after a drift event (after drift mode's camera) */
    { "showoff",     showoff_frame },          /* the car's trunk, hood and doors open */
    { "plates",      plates_frame },           /* license plates with the players' own text */
    { "lightkey",    lightkey_frame },         /* the headlights on G (off, high beam), friends' too */
    { "photo",       photo_frame },            /* photo mode (F12, the pause menu's Photo Mode) */
    { "savegame",    f_savegame },             /* the pause menu's Save, once its save is over */
    { "coop",        coop_frame },             /* co-op free roam */
    { "engage",      engage_frame },           /* Engage at a marker mended when the game would refuse it for good */
    { "roam",        roam_frame },             /* roam.c's probes and test commands */
};

void features_install(void){
    for (size_t i = 0; i < COUNT(k_installs); i++)
        if (k_installs[i].builds & FT_HERE) k_installs[i].install();
}
int features_hook(CPU *c, uint32_t ip){
    for (size_t i = 0; i < COUNT(k_hooks); i++)
        if (k_hooks[i].hook(c, ip)) return 1;
    return 0;
}
void features_frame(CPU *c){
    for (size_t i = 0; i < COUNT(k_frames); i++)
        if (k_frames[i].frame) k_frames[i].frame(c);
}
