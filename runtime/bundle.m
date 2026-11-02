/* bundle.m -- what the double-clickable NFSU2.app needs around the port.
 *
 * Everything here only runs when the binary sits inside an .app bundle
 * (".../X.app/Contents/MacOS/Y"); a terminal run of out/nfsu2_boot is
 * unchanged.  Layout (NFSU2_APPSUPPORT / NFSU2_LOGDIR move it, for tests):
 *
 *   ~/Library/Application Support/NFSU2/
 *     settings.ini   launch options, mapped onto the runtime's environment
 *                    variables (app_load_settings; template written on first
 *                    launch, every line commented out = built-in defaults)
 *     registry.txt   the game's registry (HKLM settings), seeded on first
 *                    launch from Contents/Resources/registry-defaults.txt
 *     Overlay/       every file the game writes into its own folder: profiles
 *                    ("NFS Underground 2/<NAME>/"), screenshots.  Reads look
 *                    here first and fall back to the game folder (fileio.c)
 *   ~/Library/Logs/NFSU2/nfsu2.log   (previous run: nfsu2.previous.log)
 *
 * The game folder is the user's own install and is only ever read:
 * fileio_set_readonly_root(1) makes the file layer refuse any write into it.
 * SPEED2.EXE must be the build the code was lifted from (v1.2 NA, SHA-256
 * below); any other build is refused.                                    */
#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include "enhance.h"
#include "names.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <libgen.h>
#include <mach-o/dyld.h>

#define EXE_SHA256 "f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7"
#define EXE_SIZE   4800512

static char g_exe_dir[1024];      /* .../NFSU2.app/Contents/MacOS      */
static char g_app_dir[1024];      /* the folder that CONTAINS the .app */
static char g_res_dir[1024];      /* .../NFSU2.app/Contents/Resources  */
static char g_support[1024];      /* Application Support/NFSU2         */
static char g_settings[1100];     /* .../settings.ini                  */
static char g_start_cwd[1024];    /* cwd before app_setup's chdir      */
static int  g_bundled = -1;

/* Are we running from a bundle?  We look at the real path with symbolic
 * links resolved: yes if it is ".../X.app/Contents/MacOS/Y".            */
int app_is_bundled(void){
    if (g_bundled >= 0) return g_bundled;
    char raw[1024]; uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0){ g_bundled = 0; return 0; }
    char real[1024];
    if (!realpath(raw, real)) snprintf(real, sizeof real, "%s", raw);

    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", real);
    char *s = strrchr(g_exe_dir, '/'); if (s) *s = 0;

    const char *tail = "/Contents/MacOS";
    size_t el = strlen(g_exe_dir), tl = strlen(tail);
    if (el <= tl || strcmp(g_exe_dir + el - tl, tail) != 0){ g_bundled = 0; return 0; }

    /* .../X.app  ->  Resources and the folder containing the .app */
    char app[1024]; snprintf(app, sizeof app, "%.*s", (int)(el - tl), g_exe_dir);
    snprintf(g_res_dir, sizeof g_res_dir, "%s/Contents/Resources", app);
    snprintf(g_app_dir, sizeof g_app_dir, "%s", app);
    s = strrchr(g_app_dir, '/'); if (s) *s = 0;
    g_bundled = 1;
    return 1;
}

static void mkdirs(const char *path){
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++){
        if (*q != '/') continue;
        *q = 0; mkdir(tmp, 0755); *q = '/';
    }
    mkdir(tmp, 0755);
}
static int no_ui(void){
    const char *a = getenv("NFSU2_NO_UI"), *b = getenv("NOFOCUS");
    return (a && *a && *a != '0') || (b && *b && *b != '0');
}
static void app_alert(const char *title, const char *msg){
    fprintf(stderr, "app: %s -- %s\n", title, msg);
    if (no_ui()) return;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    NSAlert *a = [NSAlert new];
    [a setMessageText:@(title)];
    [a setInformativeText:@(msg)];
    [a addButtonWithTitle:@"OK"];
    [a runModal];
}

/* winsock.c, once per run, when nothing sent beyond this Mac has worked
 * for a while: macOS is most likely refusing Local Network access.  The
 * game shows nothing of its own for a host (its game simply never appears
 * on the other machines), so the app says it: an alert as a sheet on the
 * game window (the game and its network keep running underneath) with a
 * button to that settings pane.  Only a log line outside the app and in
 * test runs (NOFOCUS / NFSU2_NO_UI). */
void app_local_network_notice(void){
    const char *t = "NFSU2 cannot reach your local network";
    const char *m = "macOS is not letting NFSU2 send anything on the local network, so the other players cannot "
                    "see your game and you cannot see theirs.\n\nTurn on NFSU2 in System Settings > Privacy & "
                    "Security > Local Network, then go back to the main menu and enter LAN again.";
    if (!app_is_bundled() || no_ui()){
        fprintf(stderr, "app: Local Network notice not shown (%s): %s\n", no_ui() ? "no UI in this run" : "not the app", t);
        return;
    }
    fprintf(stderr, "app: Local Network notice shown: %s\n", t);
    NSString *ts = @(t), *ms = @(m);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new];
        [a setMessageText:ts];
        [a setInformativeText:ms];
        [a addButtonWithTitle:@"Open Settings"];
        [a addButtonWithTitle:@"Not Now"];
        void (^done)(NSModalResponse) = ^(NSModalResponse r){
            if (r != NSAlertFirstButtonReturn) return;
            NSURL *u = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_LocalNetwork"];
            if (u) [[NSWorkspace sharedWorkspace] openURL:u];
        };
        NSWindow *w = [NSApp mainWindow];
        if (!w) w = [NSApp keyWindow];
        if (!w) w = [[NSApp windows] firstObject];
        if (w) [a beginSheetModalForWindow:w completionHandler:done];
        else done([a runModal]);
    });
}

/* ---- settings.ini ------------------------------------------------------- *
 * "key = value" lines, '#' or ';' starts a comment line, [sections] are
 * ignored.  A real environment variable always wins over the file, the file
 * wins over the built-in defaults (app_set_defaults).  Known keys are mapped
 * below; an UPPER_CASE key is exported as is (any runtime variable), any
 * other key "foo_bar" becomes NFSU2_FOO_BAR, so options added by other parts
 * of the runtime work without touching this table.                        */
/* K_ENUM: one of the '|'-separated values ("#" = any number, "scale:#" =
 * that prefix and a number); a list with "on" and "off" also takes the other
 * boolean spellings.  K_NUM: a number.  K_ID: decimal digits only (at most
 * 31).  K_TOKEN: printable ASCII without spaces, under 300 bytes (an asset
 * key or a URL).  K_INT: a whole decimal number in the range "lo..hi".
 * K_NAME: printable ASCII, 1 to 24 characters, not one that reads as a
 * slur (names.h).  K_PEERS: IPv4 literals a.b.c.d or a.b.c.d:port,
 * separated by commas, ';' or blanks (no host names).  Anything else is
 * logged and ignored, so a typo never silently selects a default.  K_IC (added to a kind): after the value, ';' or '#'
 * after a blank starts a comment ("roam_name = Tobi ; me" is Tobi,
 * "Team#1" stays Team#1), the rule the free-roam options follow in the
 * runtime and in the Windows plugin's .ini.
 * K_PATH: a folder (absolute, ~/..., or a name), printable, under 1024
 * bytes.  K_TEXT: printable text (UTF-8 allowed), 1 to 63 bytes.  K_ROOM:
 * K_TEXT that does not read as a slur (names.h, as K_NAME).  K_CREWS:
 * crew names joined by commas, printable (UTF-8 allowed), under 1024 bytes,
 * each name at most 24 characters without the blanks round it (the page's
 * crews.ts; the game takes the first 4).  K_RGBS: at most 4 colours joined
 * by commas, each 6 hex digits (no '#') or empty, blanks round them.  K_WXH:
 * <w>x<h>, each 320..16384.  K_FIXES: default|all|none or a comma list of
 * the frame-rate fixes, "-name" removes one.  K_IFNAME: a network interface
 * name (letters and digits, at most 15).  K_IPV4: a.b.c.d.  K_MAC:
 * xx:xx:xx:xx:xx:xx (hex), not all zero.  K_HOST: a computer name (letters,
 * digits and '-', at most 63).  K_PARTS: all, or a comma list of the texture
 * pack's parts.  K_DUMP: on/off or a folder (as K_PATH).  Every key of the
 * table has a kind that checks its value: K_RAW is not used. */
enum { K_RAW, K_BOOL, K_BOOL_INV, K_ENUM, K_NUM, K_ID, K_TOKEN, K_INT, K_NAME, K_PEERS,
       K_PATH, K_TEXT, K_ROOM, K_CREWS, K_RGBS, K_WXH, K_FIXES, K_IFNAME, K_IPV4, K_MAC, K_HOST, K_PARTS, K_DUMP };
#define K_IC 0x100
static const struct { const char *key, *env; int kind; const char *vals; } g_keys[] = {
    /* app */
    { "game_folder",              "NFSU2_DATA",                 K_PATH     },
    { "movies",                   "NOMOVIES",                   K_BOOL_INV },
    { "intro",                    "NFSU2_INTRO",                K_ENUM, "skippable|original|off" },
    { "sound",                    "NOSOUND",                    K_BOOL_INV },
    { "background",               "NFSU2_BACKGROUND",           K_ENUM, "pause|run" },
    { "quit_grace",               "NFSU2_QUIT_GRACE",           K_NUM      },
    { "seed_registry",            "NFSU2_SEED_REGISTRY",        K_BOOL     },
    /* stall report in the log */
    { "watchdog",                 "NFSU2_WATCHDOG",             K_NUM      },
    /* display */
    { "fullscreen",               "NFSFULL",                    K_BOOL     },
    { "native_resolution",        "NFSNATIVE",                  K_BOOL     },
    { "resolution",               "NFSRES",                     K_WXH      },
    { "render",                   "NFSU2_RENDER",               K_ENUM, "backing|panel|scale:#|#" },
    { "max_pixels",               "NFSMAXPIX",                  K_NUM      },
    { "hud",                      "NFSU2_HUD",                  K_ENUM, "wide|43|4:3" },
    { "movie_framing",            "NFSU2_MOVIE",                K_ENUM, "fit|fill|stretch" },
    { "cutscene_borders",         "NFSU2_CUTSCENE_BORDERS",     K_ENUM, "full|off" },
    /* graphics options and frame-rate counter */
    { "graphics",                 "NFSU2_GRAPHICS",             K_ENUM, "original|enhanced|custom" },
    { "msaa",                     "NFSU2_MSAA",                 K_ENUM, "off|2|4|auto" },
    { "mirror",                   "NFSU2_MIRROR",               K_ENUM, "original|high|#" },
    { "texture_filtering",        "NFSU2_TEXTURE_FILTERING",    K_ENUM, "game|anisotropic" },
    { "fps_counter",              "NFSU2_FPS_COUNTER",          K_ENUM, "off|on|detailed" },
    { "metal_hud",                "MTL_HUD_ENABLED",            K_BOOL     },
    /* frame rate */
    { "fps",                      "NFSU2_FPS",                  K_ENUM,
      "auto|30|60|120|240|uncapped|off|native|vanilla|game|max" },
    { "hfr_fixes",                "NFSU2_HFR_FIX",              K_FIXES    },
    { "nos_trail_length",         "NFSU2_NOS_TRAIL_LEN",        K_NUM      },
    /* game controller */
    { "gamepad",                  "NFSU2_PAD",                  K_ENUM, "on|off|hotplug" },
    { "gamepad_deadzone",         "NFSU2_PAD_DEADZONE",         K_NUM      },
    { "gamepad_trigger_deadzone", "NFSU2_PAD_TRIGGER_DEADZONE", K_NUM      },
    { "gamepad_trigger_click",    "NFSU2_PAD_TRIGGER_CLICK",    K_NUM      },
    { "gamepad_menu_keys",        "NFSU2_PAD_FE_KEYS",          K_BOOL     },
    { "gamepad_background",       "NFSU2_PAD_BACKGROUND",       K_BOOL     },
    /* Discord status */
    { "discord",                  "NFSU2_DISCORD",              K_BOOL     },
    { "discord_detail",           "NFSU2_DISCORD_DETAIL",       K_ENUM, "full|minimal" },
    { "discord_app_id",           "NFSU2_DISCORD_APP_ID",       K_ID       },
    { "discord_large_image",      "NFSU2_DISCORD_LARGE_IMAGE",  K_TOKEN    },
    /* free-roam multiplayer (the runtime checks the values too) */
    { "roam",                     "NFSU2_ROAM",                 K_BOOL  | K_IC },
    { "roam_name",                "NFSU2_ROAM_NAME",            K_NAME  | K_IC },
    { "roam_port",                "NFSU2_ROAM_PORT",            K_INT   | K_IC, "1024..65535" },
    { "roam_peers",               "NFSU2_ROAM_PEERS",           K_PEERS | K_IC },
    { "roam_discovery",           "NFSU2_ROAM_DISCOVERY",       K_BOOL  | K_IC },
    { "roam_room",                "NFSU2_ROAM_ROOM",            K_ROOM  | K_IC },
    { "roam_crew",                "NFSU2_ROAM_CREW",            K_CREWS | K_IC },
    { "roam_crew_colours",        "NFSU2_ROAM_CREW_RGB",        K_RGBS  | K_IC },
    { "roam_max_players",         "NFSU2_ROAM_MAX_PLAYERS",     K_INT   | K_IC, "2..49" },
    { "roam_max_cars",            "NFSU2_ROAM_MAX_CARS",        K_INT   | K_IC, "0..48" },
    { "roam_collide",             "NFSU2_ROAM_COLLIDE",         K_ENUM  | K_IC, "on|off|none" },
    { "roam_rate",                "NFSU2_ROAM_RATE",            K_INT   | K_IC, "10..30" },
    { "roam_delay",               "NFSU2_ROAM_DELAY",           K_INT   | K_IC, "0..1000" },
    { "roam_extrap",              "NFSU2_ROAM_EXTRAP",          K_INT   | K_IC, "0..1000" },
    { "roam_carpool_kb",          "NFSU2_ROAM_CARPOOL_KB",      K_INT   | K_IC, "0..128000" },
    /* LAN play */
    { "network",                  "NFSU2_NET",                  K_ENUM, "lan|on|off" },
    { "lan_interface",            "NFSU2_NET_IFACE",            K_IFNAME   },
    { "lan_address",              "NFSU2_NET_ADDR",             K_IPV4     },
    { "lan_adapter_id",           "NFSU2_NET_MAC",              K_MAC      },
    { "lan_host_name",            "NFSU2_NET_NAME",             K_HOST     },
    { "lan_timing",               "NFSU2_LAN_TIMING",           K_ENUM, "pc|exact" },
    /* texture packs */
    { "textures",                 "NFSU2_TEXTURES",             K_ENUM, "original|upscaled" },
    { "texture_pack",             "NFSU2_TEXPACK",              K_PATH     },
    { "texture_parts",            "NFSU2_TEXPARTS",             K_PARTS    },
    { "texture_dump",             "NFSU2_TEXDUMP",              K_DUMP     },
    { "texture_budget",           "NFSU2_TEXBUDGET",            K_INT, "16..65536" },
};
#define NKEYS (int)(sizeof g_keys / sizeof g_keys[0])

static const char *const k_template =
"# NFSU2 settings, read at every launch.\n"
"#\n"
"# One \"key = value\" per line; lines starting with '#' are comments.  Every\n"
"# option below is commented out and shows its built-in default: remove the\n"
"# '#' to change it.  An environment variable of the same meaning (shown in\n"
"# brackets) overrides this file.  Any other runtime variable can be set here\n"
"# too, by its own name in capitals (e.g. NFSU2_STATUS_EVERY = 600), and a\n"
"# lower-case key foo_bar is passed on as NFSU2_FOO_BAR.  A value that an\n"
"# option does not accept is ignored and noted in the log.\n"
"\n"
"# ---- Game folder and app ----------------------------------------------\n"
"\n"
"# Folder that contains SPEED2.EXE (v1.2 NA).  Filled in by the folder picker\n"
"# on first launch.  It is only ever read; nothing is written into it.  [NFSU2_DATA]\n"
"#game_folder = /path/to/Need for Speed Underground 2\n"
"\n"
"# Intro, story and FMV movies.  [NOMOVIES, inverted]\n"
"#movies = on\n"
"\n"
"# The movies at startup (EA and THX logos, the PSA, the intro): skippable =\n"
"# Return, Escape, Space, a left click or a controller's A, B or Start ends each\n"
"# one; original = as in the game, where only the intro ends early; off = they\n"
"# are not played.  [NFSU2_INTRO]\n"
"#intro = skippable\n"
"\n"
"# Sound output.  [NOSOUND, inverted]\n"
"#sound = on\n"
"\n"
"# What the game does while another app is active: \"pause\" freezes it and\n"
"# pauses its sound, like alt-tab on Windows; \"run\" keeps it going.  In the\n"
"# LAN screens it always keeps running (sound paused), or the other players\n"
"# would drop you.  [NFSU2_BACKGROUND]\n"
"#background = pause\n"
"\n"
"# Seconds the game gets to shut down after Cmd-Q / closing the window before\n"
"# the app exits anyway (open save files are always finished first).  [NFSU2_QUIT_GRACE]\n"
"#quit_grace = 10\n"
"\n"
"# Seed registry.txt with the game's highest graphics settings on first launch\n"
"# (native-resolution row).  off = let the game run its own first-run hardware\n"
"# check instead.  Delete registry.txt to start over.  [NFSU2_SEED_REGISTRY]\n"
"#seed_registry = on\n"
"\n"
"# Seconds without a new frame (outside loading screens) before the log gets\n"
"# a report of what every game thread is doing, once per stall; 0 = off.\n"
"# Never while the game is paused in the background.  [NFSU2_WATCHDOG]\n"
"#watchdog = 15\n"
"\n"
"# ---- Display -----------------------------------------------------------\n"
"\n"
"# Start in full screen (on) or in a window (off).  Empty = remember the last\n"
"# choice (Cmd-F); on or off also stops it being remembered.  [NFSFULL]\n"
"#fullscreen =\n"
"\n"
"# A render size the game's resolution list does not have (or any, with\n"
"# NOWIDE=1) is raised to the display's native pixel count, keeping the\n"
"# game's aspect.  [NFSNATIVE]\n"
"#native_resolution = on\n"
"# Render every resolution at this fixed size instead, e.g. 2560x1600;\n"
"# empty = the one chosen in the game.  [NFSRES]\n"
"#resolution =\n"
"\n"
"# Render size of the native-resolution row: backing = the window's backing\n"
"# pixels 1:1; panel = the physical panel's pixel density (fewer pixels in a\n"
"# scaled \"More Space\" mode: cheaper, slightly softer); scale:<f> = backing\n"
"# pixels times f (0.25 to 2).  [NFSU2_RENDER]\n"
"#render = backing\n"
"# Upper bound for every native render size, in pixels (0 = none).  [NFSMAXPIX]\n"
"#max_pixels = 9000000\n"
"\n"
"# Race HUD: wide = anchored to the screen edges (to a centred 16:9 area on\n"
"# wider screens); 43 = the original centred 4:3 layout.  [NFSU2_HUD]\n"
"#hud = wide\n"
"# Movies: fit = pillarboxed 4:3 as authored; fill = cover the screen (crops\n"
"# top and bottom); stretch = widened to the screen.  [NFSU2_MOVIE]\n"
"#movie_framing = fit\n"
"# Cutscene letterbox bars: full = across the whole width; off = none.\n"
"# [NFSU2_CUTSCENE_BORDERS]\n"
"#cutscene_borders = full\n"
"\n"
"# ---- Graphics ----------------------------------------------------------\n"
"\n"
"# Every option here is off by default: the picture is the game's own, pixel\n"
"# for pixel.  original = that look, whatever msaa, mirror,\n"
"# texture_filtering and textures say (fps_counter and metal_hud still work);\n"
"# enhanced = the recommended set (msaa auto, mirror high, texture_filtering anisotropic;\n"
"# a line below that is set overrides its part; with a texture pack built,\n"
"# also textures = upscaled unless the Textures line says original); custom =\n"
"# only the lines below.  Without this line: custom if one of them is set,\n"
"# else original.  [NFSU2_GRAPHICS]\n"
"#graphics = original\n"
"# Anti-aliasing of the 3D scene: off, 2, 4 (samples), or auto = 2 at 1400\n"
"# rows and more, 4 below.  2x costs about 1 ms per frame in thick tyre smoke.\n"
"# [NFSU2_MSAA]\n"
"#msaa = off\n"
"# Rear-view mirror (bumper and hood cameras): original = 256x256; high =\n"
"# 1024x1024 (or a size in pixels).  [NFSU2_MIRROR]\n"
"#mirror = original\n"
"# game = the game's own texture filtering setting; anisotropic = 16x\n"
"# anisotropic filtering, as the game's own \"anisotropic\" setting gives,\n"
"# without changing the saved game options.  [NFSU2_TEXTURE_FILTERING]\n"
"#texture_filtering = game\n"
"# Frame-rate counter in the top-left corner: on = frames per second, frame\n"
"# time and a frame-time graph; detailed = also the 1% low, GPU time, pacing\n"
"# and render size.  Cmd-N cycles off / on / detailed while playing.\n"
"# [NFSU2_FPS_COUNTER]\n"
"#fps_counter = off\n"
"# Apple's Metal Performance HUD (frame times, GPU time, memory), drawn by\n"
"# macOS itself.  [MTL_HUD_ENABLED]\n"
"#metal_hud = off\n"
"\n"
"# ---- Frame rate --------------------------------------------------------\n"
"\n"
"# auto = follow the display's refresh rate (60, 120 or 240; 144/165 Hz give\n"
"# 120); 30, 60, 120 or 240 = fixed (30: steady frames where 60 does not\n"
"# hold, as on a slow phone); uncapped = no pacing (benchmark only);\n"
"# off = the game's own 1/120 s limiter, the timing before the frame-rate\n"
"# work.  Cutscenes stay at 30 fps in every mode.  [NFSU2_FPS]\n"
"#fps = auto\n"
"# Fixes for subsystems that count frames: default (all above 60 fps, none\n"
"# at 60, ticks,particles,rain,spinners at 30), all, none, or a list of\n"
"# ticks,timer,particles,trail,audio,rtf,rain,spinners (a leading '-' removes\n"
"# one, e.g. default,-audio).  [NFSU2_HFR_FIX]\n"
"#hfr_fixes = default\n"
"# Length factor of the NOS / streak trail.  [NFSU2_NOS_TRAIL_LEN]\n"
"#nos_trail_length = 1.0\n"
"\n"
"# ---- Game controller ---------------------------------------------------\n"
"\n"
"# on = the game sees a controller once one has been connected, and keeps it;\n"
"# hotplug = only while one is connected (DirectInput behaviour); off = no\n"
"# controller.  The bindings are set in the game's Controls menu.  [NFSU2_PAD]\n"
"#gamepad = on\n"
"# Stick dead zone (fraction of full travel, 0 to 0.9) and trigger dead zone.\n"
"# [NFSU2_PAD_DEADZONE, NFSU2_PAD_TRIGGER_DEADZONE]\n"
"#gamepad_deadzone = 0.08\n"
"#gamepad_trigger_deadzone = 0.02\n"
"# Trigger travel at which the full-throttle / full-brake buttons press.\n"
"# [NFSU2_PAD_TRIGGER_CLICK]\n"
"#gamepad_trigger_click = 0.97\n"
"# A, B and View also send Enter, Escape and Q in the menus.  [NFSU2_PAD_FE_KEYS]\n"
"#gamepad_menu_keys = on\n"
"# Keep reading the controller while another app is active.  [NFSU2_PAD_BACKGROUND]\n"
"#gamepad_background = off\n"
"\n"
"# ---- Discord ------------------------------------------------------------\n"
"\n"
"# While the Discord app is open, your Discord profile shows that you are\n"
"# playing NFSU2 and what you are doing (menus, race type and track, lap and\n"
"# position, career stage), for your friends to see.  off = nothing is sent.\n"
"# Nothing ever leaves this Mac except through the Discord app.  [NFSU2_DISCORD]\n"
"#discord = on\n"
"# full, or minimal = only that you are playing and for how long.\n"
"# [NFSU2_DISCORD_DETAIL]\n"
"#discord_detail = full\n"
"# Your own Discord application id (digits), if Discord refuses the built-in\n"
"# one (Discord's own \"Need for Speed: Underground 2\" entry).  [NFSU2_DISCORD_APP_ID]\n"
"#discord_app_id =\n"
"# Large image: an asset key of your own application, or an https URL of an\n"
"# image you may use.  Empty = none.  [NFSU2_DISCORD_LARGE_IMAGE]\n"
"#discord_large_image =\n"
"\n"
"# ---- Free-roam multiplayer ---------------------------------------------\n"
"\n"
"# Friends on the same network (Wi-Fi or LAN), each in their own career free\n"
"# roam, see each other's cars: model, parts and paint, about 0.1 s behind,\n"
"# with an arrow on the minimap; you drive through them.  Traffic, races and\n"
"# money stay your own.  macOS asks once for Local Network access: allow it.\n"
"# Friends on Windows need the NFSU2FreeRoam.asi plugin.  For these options\n"
"# a ';' or '#' after a blank starts a comment.\n"
"# [NFSU2_ROAM]\n"
"#roam = off\n"
"# The name your friends see (printable ASCII, at most 24 characters).\n"
"# Empty = your profile name.  [NFSU2_ROAM_NAME]\n"
"#roam_name =\n"
"# Friends' IPv4 addresses, if the automatic search does not find them (some\n"
"# routers, VPNs): a.b.c.d or a.b.c.d:port, separated by commas; one friend is\n"
"# enough, the others are passed on.  [NFSU2_ROAM_PEERS]\n"
"#roam_peers =\n"
"# Empty: everyone.  With a name, only the players who set the same name\n"
"# (capitals and spaces do not count).  [NFSU2_ROAM_ROOM]\n"
"#roam_room =\n"
"# Your crews, up to 4, joined by commas (Blue Room, Night Owls): players who\n"
"# share one see each other's names and map arrows in its colour, at any\n"
"# distance, with everyone else still there.  [NFSU2_ROAM_CREW]\n"
"#roam_crew =\n"
"# Each crew's colour, in the order of roam_crew: 6 hex digits, no #.  Empty:\n"
"# cyan, magenta, violet, red.  [NFSU2_ROAM_CREW_RGB]\n"
"#roam_crew_colours =\n"
"# UDP port, the same on every machine (1024 to 65535).  [NFSU2_ROAM_PORT]\n"
"#roam_port = 47210\n"
"# Look for friends with a network broadcast every 2 s.  [NFSU2_ROAM_DISCOVERY]\n"
"#roam_discovery = on\n"
"# Largest group, you included (2 to 49), and friends' cars shown at once\n"
"# (0 to 48, the nearest).  [NFSU2_ROAM_MAX_PLAYERS, NFSU2_ROAM_MAX_CARS]\n"
"#roam_max_players = 49\n"
"#roam_max_cars = 32\n"
"# on = friends' cars are solid: drawn where they are now, and one that\n"
"# appears on your car lets you through until you are apart.  off = ghosts,\n"
"# and your car one for your friends.  none = ghosts, and traffic and the\n"
"# game's tuners pass through you too (not in races, not a tuner you race\n"
"# in an outrun).  [NFSU2_ROAM_COLLIDE]\n"
"#roam_collide = on\n"
"# Snapshots per second (10 to 30), the delay ghost cars are shown with in\n"
"# ms (0 to 1000) and the longest cars are extrapolated in ms (0 to 1000).\n"
"# [NFSU2_ROAM_RATE, NFSU2_ROAM_DELAY, NFSU2_ROAM_EXTRAP]\n"
"#roam_rate = 20\n"
"#roam_delay = 100\n"
"#roam_extrap = 250\n"
"# The free-roam car memory pool in KB, raised for friends' cars (the game's\n"
"# own is 11000; a value at or below it is ignored, as it would leave no room\n"
"# for them).  [NFSU2_ROAM_CARPOOL_KB]\n"
"#roam_carpool_kb = 48000\n"
"\n"
"# ---- LAN play ----------------------------------------------------------\n"
"\n"
"# LAN races with other PCs and Macs on the same network (main menu > LAN).\n"
"# lan = on; off = no network at all.  The first LAN search makes macOS ask\n"
"# for Local Network access: allow it (System Settings > Privacy & Security >\n"
"# Local Network).  The network is looked at each time you enter LAN from\n"
"# the main menu: after changing networks, go back to the main menu.  [NFSU2_NET]\n"
"#network = lan\n"
"# Network interface for LAN play when the Mac has several (e.g. en0 or en1;\n"
"# see System Settings > Network > Details or ifconfig).  A self-assigned\n"
"# 169.254 address counts, e.g. for a cable straight to the PC.  Empty = the\n"
"# one with the default route (never a VPN tunnel).  [NFSU2_NET_IFACE]\n"
"#lan_interface =\n"
"# Or this Mac's own IPv4 address on that network: its interface is used.\n"
"# An address the Mac does not have is ignored (see the log).  [NFSU2_NET_ADDR]\n"
"#lan_address =\n"
"# Adapter id (MAC, xx:xx:xx:xx:xx:xx) the game sends as its machine id.\n"
"# Empty = the interface's hardware address, or one derived from this Mac.\n"
"# [NFSU2_NET_MAC]\n"
"#lan_adapter_id =\n"
"# Computer name the game sees (letters, digits and '-').  Empty = this\n"
"# Mac's name.  [NFSU2_NET_NAME]\n"
"#lan_host_name =\n"
"# Clocks in a LAN race: pc = the game's own timing, as on an unmodified PC\n"
"# at the same frame rate; exact = keep the frame-rate fixes (equal between\n"
"# Macs at any fps, about 0.5 % off against any PC).  A machine at 120 fps\n"
"# gets about 1 % more race time per second than one at 60 (the game's own\n"
"# timing, on Windows too): for an even race give every machine the same\n"
"# fps (fps = 60 here against a 60 Hz PC).  With pc, fps = 240 races at\n"
"# 120, the fastest an unmodified PC runs.  [NFSU2_LAN_TIMING]\n"
"#lan_timing = pc\n"
"\n"
"# ---- Textures ----------------------------------------------------------\n"
"\n"
"# original = the game's own textures.  upscaled = use a texture pack built\n"
"# on this Mac from your game files with tools/texpack (sharper roads,\n"
"# walls, signs and cars up close; distant surfaces stay the game's own).\n"
"# Without a pack nothing changes.  graphics = enhanced turns this on too\n"
"# (unless this line says original), graphics = original turns it off.\n"
"# Cmd-T shows / hides the pack while playing.  [NFSU2_TEXTURES]\n"
"#textures = original\n"
"# The pack: a folder name in Textures/ next to this file, or a full path.\n"
"# [NFSU2_TEXPACK]\n"
"#texture_pack = default\n"
"# Parts of the pack to use, or all: world, signs, cars, garage, sky,\n"
"# effects, frontend (menus), hud, other.  [NFSU2_TEXPARTS]\n"
"#texture_parts = world,signs,cars,garage\n"
"# GPU memory for the pack in MB (16 to 65536); beyond it the game's own\n"
"# textures are used (empty = a sixth of the Mac's memory, at most 4096).\n"
"# [NFSU2_TEXBUDGET]\n"
"#texture_budget =\n"
"# on = save every texture the game loads into Textures/dump (or a folder\n"
"# given here), for tools/texpack: menus and cars come from there.\n"
"# [NFSU2_TEXDUMP]\n"
"#texture_dump = off\n";

static int truthy(const char *v){
    return !strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") ||
           !strcasecmp(v, "true") || !strcasecmp(v, "enabled");
}
static int falsy(const char *v){
    return !strcasecmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "no") ||
           !strcasecmp(v, "false") || !strcasecmp(v, "disabled");
}
static char *trim(char *s){
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') || (s[0] == '\'' && s[n-1] == '\''))){
        s[n-1] = 0; s++; }
    return s;
}
/* Messages are kept until the log is open (the file is read before it). */
static char g_set_log[4096]; static size_t g_set_logn;
static void set_note(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    if (g_set_logn < sizeof g_set_log)
        g_set_logn += (size_t)vsnprintf(g_set_log + g_set_logn, sizeof g_set_log - g_set_logn, fmt, ap);
    va_end(ap);
    if (g_set_logn > sizeof g_set_log) g_set_logn = sizeof g_set_log;
}
static int is_number(const char *v){
    char *end = 0;
    if (!*v) return 0;
    (void)strtod(v, &end);
    return end && end != v && !*end;
}
/* K_ENUM: the accepted spelling of val, written to out (lower case as in
 * the list, or val itself for numbers); 0 if val is not in the list. */
static int enum_value(const char *vals, const char *val, char *out, size_t outsz){
    char list[128]; snprintf(list, sizeof list, "%s", vals);
    int has_on = 0, has_off = 0; char *save = 0;
    for (char *t = strtok_r(list, "|", &save); t; t = strtok_r(0, "|", &save)){
        size_t l = strlen(t);
        if (!strcmp(t, "on")) has_on = 1;
        if (!strcmp(t, "off")) has_off = 1;
        if (l && t[l-1] == '#'){                    /* "#" or "prefix#" */
            if (!strncasecmp(val, t, l - 1) && is_number(val + l - 1)){
                snprintf(out, outsz, "%s", val); return 1; }
        } else if (!strcasecmp(val, t)){ snprintf(out, outsz, "%s", t); return 1; }
    }
    if (has_on && truthy(val)){ snprintf(out, outsz, "on"); return 1; }
    if (has_off && falsy(val)){ snprintf(out, outsz, "off"); return 1; }
    return 0;
}
/* K_INT: a whole decimal number within "lo..hi" */
static int int_in(const char *range, const char *v){
    long lo = 0, hi = 0; char *e = 0;
    if (sscanf(range, "%ld..%ld", &lo, &hi) != 2) return 0;
    if (!*v) return 0;
    long x = strtol(v, &e, 10);
    return e && e != v && !*e && x >= lo && x <= hi;
}
/* K_PEERS: every entry an IPv4 literal (a.b.c.d or a.b.c.d:port); the first
 * bad entry goes to bad */
static int peers_ok(const char *v, char *bad, size_t badsz){
    const char *p = v; int n = 0;
    while (*p){
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        char tok[64]; size_t k = 0;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t'){ if (k < sizeof tok - 1) tok[k++] = *p; p++; }
        tok[k] = 0;
        unsigned a, b, c, d, pt = 47210; char extra; int got = 0;
        int digits = 1;
        for (const char *q = tok; *q; q++) if (!(isdigit((unsigned char)*q) || *q == '.' || *q == ':')) digits = 0;
        if (digits) got = sscanf(tok, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &pt, &extra);
        if ((got != 4 && got != 5) || a > 255 || b > 255 || c > 255 || d > 255 || pt == 0 || pt > 65535 ||
            (got == 4 && strchr(tok, ':'))){
            snprintf(bad, badsz, "%s", tok); return 0; }
        n++;
    }
    return n > 0;
}
/* the checks of the kinds above that need more than a line */
static int printable_ok(const char *v, size_t maxlen){       /* no control characters (UTF-8 bytes allowed) */
    size_t n = strlen(v);
    if (!n || n > maxlen) return 0;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) if (*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}
static int list_ok(const char *v, const char *const *words, int minus){   /* comma/blank list of words */
    char b[256]; snprintf(b, sizeof b, "%s", v); char *sv = 0; int n = 0;
    if (strlen(v) >= sizeof b) return 0;
    for (char *t = strtok_r(b, ", \t", &sv); t; t = strtok_r(0, ", \t", &sv)){
        if (minus && *t == '-') t++;
        int k = 0; while (words[k] && strcasecmp(words[k], t)) k++;
        if (!words[k]) return 0;
        n++;
    }
    return n > 0;
}
static int value_ok(int kind, const char *v){
    switch (kind){
    case K_PATH: return printable_ok(v, 1023);
    case K_TEXT: case K_ROOM: return printable_ok(v, 63);
    case K_CREWS: { if (!printable_ok(v, 1023)) return 0;
                    for (const char *p = v; *p; p += *p == ','){
                        const char *e = p; while (*e && *e != ',') e++;
                        while (p < e && strchr(" \t\n\v\f\r", *p)) p++;
                        while (e > p && strchr(" \t\n\v\f\r", e[-1])) e--;
                        int chars = 0;
                        for (; p < e; p++) chars += ((unsigned char)*p & 0xc0) != 0x80;    /* (UTF-8: a character's first byte) */
                        if (chars > 24) return 0;
                        while (*p && *p != ',') p++;
                    }
                    return 1; }
    case K_RGBS: { int n = 0;
                   for (const char *p = v; ; p++){
                       while (*p == ' ' || *p == '\t') p++;
                       int d = 0; while (isxdigit((unsigned char)*p)) { p++; d++; }
                       while (*p == ' ' || *p == '\t') p++;
                       if ((d != 0 && d != 6) || (*p && *p != ',') || ++n > 4) return 0;
                       if (!*p) return 1;
                   } }
    case K_WXH: { char *e; long w = strtol(v, &e, 10); if (e == v || (*e != 'x' && *e != 'X')) return 0;
                  const char *h0 = e + 1; long h = strtol(h0, &e, 10);
                  return e != h0 && !*e && w >= 320 && w <= 16384 && h >= 320 && h <= 16384; }
    case K_FIXES: { static const char *const w[] = { "default", "all", "none", "ticks", "timer", "particles",
                                                     "trail", "audio", "rtf", "rain", "spinners", 0 };
                    return list_ok(v, w, 1); }
    case K_IFNAME: { size_t n = strlen(v); if (!n || n > 15) return 0;
                     for (const char *p = v; *p; p++) if (!isalnum((unsigned char)*p)) return 0;
                     return 1; }
    case K_IPV4: { unsigned a[4]; char t; if (sscanf(v, "%u.%u.%u.%u%c", &a[0], &a[1], &a[2], &a[3], &t) != 4) return 0;
                   for (int i = 0; i < 4; i++) if (a[i] > 255) return 0;
                   for (const char *p = v; *p; p++) if (!isdigit((unsigned char)*p) && *p != '.') return 0;
                   return 1; }
    case K_MAC: { unsigned m[6]; int n = 0; char t;
                  if (sscanf(v, "%2x:%2x:%2x:%2x:%2x:%2x%n%c", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &n, &t) != 6
                      || (size_t)n != strlen(v)) return 0;
                  for (const char *p = v; *p; p++) if (!isxdigit((unsigned char)*p) && *p != ':') return 0;
                  return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0; }
    case K_HOST: { size_t n = strlen(v); if (!n || n > 63) return 0;
                   for (const char *p = v; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') return 0;
                   return 1; }
    case K_PARTS: { static const char *const w[] = { "all", "world", "signs", "cars", "garage", "sky", "effects",
                                                     "frontend", "hud", "other", 0 };
                    return list_ok(v, w, 0); }
    case K_DUMP: return truthy(v) || falsy(v) || printable_ok(v, 1023);
    default: return 1;
    }
}
static const char *kind_text(int kind){
    switch (kind){
    case K_PATH: return "a folder (printable, under 1024 bytes)";
    case K_TEXT: case K_ROOM: return "text of 1 to 63 bytes without control characters";
    case K_CREWS: return "crew names (at most 24 characters each) joined by commas";
    case K_RGBS: return "at most 4 colours of 6 hex digits (no #) joined by commas";
    case K_WXH: return "<width>x<height> (320 to 16384 each)";
    case K_FIXES: return "default, all, none or a list of ticks, timer, particles, trail, audio, rtf, rain (-name removes one)";
    case K_IFNAME: return "a network interface name (letters and digits, e.g. en0)";
    case K_IPV4: return "an IPv4 address a.b.c.d";
    case K_MAC: return "xx:xx:xx:xx:xx:xx (hex, not all zero)";
    case K_HOST: return "a computer name (letters, digits and '-', at most 63)";
    case K_PARTS: return "all, or a list of world, signs, cars, garage, sky, effects, frontend, hud, other";
    case K_DUMP: return "on, off or a folder";
    default: return "valid";
    }
}
static void apply_setting(const char *key, const char *val, int line){
    char env[96], norm[128], vbuf[1200]; const char *out = val; int kind = K_RAW;
    int k = 0;
    for (; k < NKEYS; k++) if (!strcasecmp(g_keys[k].key, key)) break;
    if (k < NKEYS){ snprintf(env, sizeof env, "%s", g_keys[k].env); kind = g_keys[k].kind; }
    else {
        int upper = 1;
        for (const char *p = key; *p; p++)
            if (!(isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_')){ upper = 0; break; }
        if (upper) snprintf(env, sizeof env, "%s", key);
        else { snprintf(env, sizeof env, "NFSU2_%s", key);
               for (char *p = env; *p; p++) *p = (char)toupper((unsigned char)*p); }
    }
    if (kind & K_IC){                                    /* "value ; comment" */
        size_t n = 0;
        for (size_t i = 0; val[i] && n < sizeof vbuf - 1; i++){
            if ((val[i] == ';' || val[i] == '#') && (i == 0 || val[i - 1] == ' ' || val[i - 1] == '\t')) break;
            vbuf[n++] = val[i];
        }
        while (n && (vbuf[n - 1] == ' ' || vbuf[n - 1] == '\t')) n--;
        vbuf[n] = 0; val = out = vbuf; kind &= ~K_IC;
    }
    if (!*val) return;                                   /* "key =" = default */
    if (kind == K_BOOL || kind == K_BOOL_INV){
        int t = truthy(val), f = falsy(val);
        if (!t && !f){ set_note("settings.ini:%d: %s = \"%s\" is not on/off, ignored\n", line, key, val); return; }
        out = ((kind == K_BOOL) == !!t) ? "1" : "0";
    } else if (kind == K_ENUM){
        if (!enum_value(g_keys[k].vals, val, norm, sizeof norm)){
            set_note("settings.ini:%d: %s = \"%s\" is not one of %s, ignored\n", line, key, val, g_keys[k].vals);
            return; }
        out = norm;
    } else if (kind == K_NUM && !is_number(val)){
        set_note("settings.ini:%d: %s = \"%s\" is not a number, ignored\n", line, key, val); return;
    } else if (kind == K_INT && !int_in(g_keys[k].vals, val)){
        set_note("settings.ini:%d: %s = \"%s\" is not a whole number in %s, ignored\n", line, key, val, g_keys[k].vals);
        return;
    } else if (kind == K_NAME){
        size_t n = strlen(val); int ok = n >= 1 && n <= 24;
        for (const char *p = val; *p && ok; p++) ok = *p >= ' ' && *p <= '~';
        if (!ok){ set_note("settings.ini:%d: %s = \"%s\" is not a name (printable ASCII, at most 24 characters), ignored\n",
                           line, key, val); return; }
        if (name_blocked(val)){ set_note("settings.ini:%d: %s = \"%s\" is not a name (it reads as a slur), ignored\n", line, key, val);
                                return; }
    } else if (kind == K_PEERS){
        char bad[64] = "";
        if (!peers_ok(val, bad, sizeof bad)){
            set_note("settings.ini:%d: %s = \"%s\": \"%s\" is not a.b.c.d or a.b.c.d:port (IPv4 numbers, no names), ignored\n",
                     line, key, val, bad); return; }
    } else if (kind >= K_PATH){
        if (!value_ok(kind, val)){
            set_note("settings.ini:%d: %s = \"%s\" is not %s, ignored\n", line, key, val, kind_text(kind)); return; }
        if (kind == K_ROOM && name_blocked(val)){
            set_note("settings.ini:%d: %s = \"%s\" is not a room name (it reads as a slur), ignored\n", line, key, val); return; }
        if (kind == K_DUMP && (truthy(val) || falsy(val))) out = truthy(val) ? "on" : "off";
    } else if (kind == K_ID || kind == K_TOKEN){
        size_t n = strlen(val); int ok = kind == K_ID ? n < 32 : n < 300;
        for (const char *p = val; *p && ok; p++)
            ok = kind == K_ID ? (*p >= '0' && *p <= '9') : (*p > ' ' && *p <= '~');
        if (!ok){ set_note("settings.ini:%d: %s = \"%s\" is not %s, ignored\n", line, key, val,
                           kind == K_ID ? "a numeric id" : "an asset key or URL (printable ASCII, no spaces)");
                  return; }
    }
    if (getenv(env)){ set_note("settings.ini:%d: %s ignored, %s is set in the environment\n", line, key, env); return; }
    setenv(env, out, 1);
    set_note("settings.ini:%d: %s -> %s=%s\n", line, key, env, out);
}
/* The key a line sets or shows commented out ("key = ...", "#key = ..."),
 * as an index into g_keys, or -1. */
static int line_key(const char *line){
    char b[1200]; snprintf(b, sizeof b, "%s", line);
    char *p = trim(b);
    while (*p == '#' || *p == ';') p++;
    char *eq = strchr(p, '=');
    if (!eq) return -1;
    *eq = 0; p = trim(p);
    for (int k = 0; k < NKEYS; k++) if (!strcasecmp(g_keys[k].key, p)) return k;
    return -1;
}
/* A settings.ini written by an older version of the app (it starts with the
 * template's first line) lacks the options added since: append their
 * template paragraphs, commented out like the rest, so the player can see
 * them (Discord presence is on by default).  Nothing else in the file
 * changes, and no option changes its value. */
static void settings_add_missing(const char *seen){
    char add[sizeof g_keys / sizeof g_keys[0]] = { 0 }; int n = 0;
    for (int k = 0; k < NKEYS; k++) if (!seen[k]){ add[k] = 1; n++; }
    if (!n) return;
    FILE *w = fopen(g_settings, "a");
    if (!w) return;
    fputs("\n# ---- Added by a newer version of the app (options this file did not list yet) ----\n", w);
    char names[600] = ""; size_t nl = 0;
    /* the template's paragraphs (blank-line separated) that show a missing key */
    const char *t = k_template;
    while (*t){
        const char *e = strstr(t, "\n\n"); size_t len = e ? (size_t)(e - t) + 1 : strlen(t);
        int want = 0;
        for (const char *l = t; l < t + len; ){
            const char *le = memchr(l, '\n', (size_t)(t + len - l)); size_t ll = le ? (size_t)(le - l) : (size_t)(t + len - l);
            char one[300]; snprintf(one, sizeof one, "%.*s", (int)(ll < sizeof one ? ll : sizeof one - 1), l);
            int k = line_key(one);
            if (k >= 0 && add[k]){
                want = 1; add[k] = 0;
                if (nl < sizeof names) nl += (size_t)snprintf(names + nl, sizeof names - nl, "%s%s", nl ? ", " : "", g_keys[k].key);
            }
            l += ll + 1;
        }
        if (want){ fputc('\n', w); fwrite(t, 1, len, w); }
        t += len; while (*t == '\n') t++;
    }
    fclose(w);
    set_note("settings: added the new options %s to %s (commented out, defaults unchanged)\n", names, g_settings);
}
static void app_load_settings(void){
    FILE *f = fopen(g_settings, "r");
    if (!f){
        FILE *w = fopen(g_settings, "w");
        if (w){ fputs(k_template, w); fclose(w); set_note("settings: wrote template %s\n", g_settings); }
        return;
    }
    char line[1200]; int ln = 0, ours = 0;
    char seen[sizeof g_keys / sizeof g_keys[0]] = { 0 };
    size_t first = strcspn(k_template, "\n");
    while (fgets(line, sizeof line, f)){
        ln++;
        if (ln == 1) ours = !strncmp(line, k_template, first);
        { int k = line_key(line); if (k >= 0) seen[k] = 1; }
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        char *eq = strchr(p, '=');
        if (!eq){ set_note("settings.ini:%d: no '=', ignored\n", ln); continue; }
        *eq = 0;
        char key[96]; snprintf(key, sizeof key, "%s", trim(p));
        apply_setting(key, trim(eq + 1), ln);
    }
    fclose(f);
    if (ours) settings_add_missing(seen);
}
/* Set one key in settings.ini, keeping every other line: the first line
 * that sets (or comments out) the key is replaced, else it is appended. */
static void settings_put(const char *key, const char *val){
    NSString *cur = [NSString stringWithContentsOfFile:@(g_settings) encoding:NSUTF8StringEncoding error:nil];
    NSMutableArray *lines = [NSMutableArray arrayWithArray:
        [(cur ? cur : @(k_template)) componentsSeparatedByString:@"\n"]];
    NSString *nl = [NSString stringWithFormat:@"%s = %s", key, val];
    BOOL done = NO;
    for (NSUInteger i = 0; i < lines.count && !done; i++){
        NSString *t = [lines[i] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([t hasPrefix:@"#"]) t = [[t substringFromIndex:1]
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSRange eq = [t rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *k = [[t substringToIndex:eq.location]
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([k caseInsensitiveCompare:@(key)] == NSOrderedSame){ lines[i] = nl; done = YES; }
    }
    if (!done) [lines addObject:nl];
    NSString *out = [lines componentsJoinedByString:@"\n"];
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", g_settings);
    if ([out writeToFile:@(tmp) atomically:NO encoding:NSUTF8StringEncoding error:nil])
        rename(tmp, g_settings);
    fprintf(stderr, "settings: %s = %s saved to %s\n", key, val, g_settings);
}

/* ---- logs, defaults, registry, overlay -------------------------------- */

/* Started from Finder, stderr goes to /dev/null; when a bug report is
 * needed we would have nothing.  Run from a terminal (stderr is a tty)
 * or with NFSU2_LOG given (autotest), we leave it alone.               */
static void app_open_log(void){
    const char *nl = getenv("NFSU2_LOG");
    if (isatty(2) || (nl && *nl)) return;
    char dir[1100], pth[1200], prev[1300];
    const char *ld = getenv("NFSU2_LOGDIR"), *home = getenv("HOME");
    if (ld && *ld) snprintf(dir, sizeof dir, "%s", ld);
    else if (getenv("NFSU2_APPSUPPORT")) snprintf(dir, sizeof dir, "%s/Logs", g_support);
    else if (home) snprintf(dir, sizeof dir, "%s/Library/Logs/NFSU2", home);
    else return;
    mkdirs(dir);
    /* The HD build writes its own log, so two builds run side by side do not
     * overwrite each other's log.  Fresh on every run; the previous is kept. */
    const char *lg = NFS_ENHANCED ? "nfsu2-hd" : "nfsu2";
    snprintf(pth, sizeof pth, "%s/%s.log", dir, lg);
    snprintf(prev, sizeof prev, "%s/%s.previous.log", dir, lg);
    rename(pth, prev);
    int fd = open(pth, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    dup2(fd, 1); dup2(fd, 2); close(fd);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
}

/* The in-bundle counterpart of the settings run.sh exports.  Left alone if
 * already set in the environment or in settings.ini.  ALARM is 0 on purpose:
 * boot.c's 20 s diagnostic timeout would take the game away from the user. */
static void setdef(const char *k, const char *v){ if (!getenv(k)) setenv(k, v, 1); }
static void app_set_defaults(void){
    setdef("WINDOW", "1");
    setdef("ALARM", "0");
    setdef("FIXMAT", "0");       /* v12na: not needed (see run.sh) */
    setdef("FIXMAT_MODE", "3");
    setdef("NOMOVIES", "0");     /* v12na: the VP6 movies work */
    setdef("SNDSYNC", "1");
    setdef("GPUVS", "1");
    setdef("NFSU2_READONLY_DATA", "1");   /* never write into the game folder */
}

/* Save overlay for the file layer (boot.c): NFSU2_SAVEDIR, which app_setup
 * points at Overlay/ and tools/autorun.sh at a per-run directory. */
const char *app_save_dir(void){
    const char *env = getenv("NFSU2_SAVEDIR");
    return (env && *env) ? env : NULL;
}

static int copy_small(const char *a, const char *b){
    FILE *fa = fopen(a, "rb"); if (!fa) return 0;
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", b);
    FILE *fb = fopen(tmp, "wb"); if (!fb){ fclose(fa); return 0; }
    char buf[8192]; size_t k;
    while ((k = fread(buf, 1, sizeof buf, fa)) > 0) fwrite(buf, 1, k, fb);
    fclose(fa);
    if (fclose(fb) != 0){ unlink(tmp); return 0; }
    return rename(tmp, b) == 0;
}
/* First launch: registry.txt = registry-defaults.txt, i.e. what the game's
 * own first-run check writes on this runtime (it always picks performance
 * level 4: the GPU is reported as a GeForce 10de:0040 and rdtsc runs at 3
 * GHz, so the check is deterministic), raised to the highest the game's
 * options go (performance level 5, car reflections 3, world detail 3,
 * texture filtering 2), with g_RacingResolution=3 (the
 * 1280x960 row, which the runtime maps to the display's native pixels;
 * the check itself would write row 1).  NotFirstTime=1 is part of that
 * state.  seed_registry = off, or deleting registry.txt with the seed
 * removed from the bundle, lets the game run its check instead. */
static void app_seed_registry(const char *reg){
    struct stat st;
    if (stat(reg, &st) == 0) return;
    const char *e = getenv("NFSU2_SEED_REGISTRY");
    if (e && *e == '0'){ fprintf(stderr, "app: no registry yet, the game will run its first-run check\n"); return; }
    char src[1200]; snprintf(src, sizeof src, "%s/registry-defaults.txt", g_res_dir);
    if (copy_small(src, reg)) fprintf(stderr, "app: first launch, registry seeded from %s\n", src);
    else fprintf(stderr, "app: cannot seed %s from %s: %s\n", reg, src, strerror(errno));
}

/* Called first thing in main() when bundled. */
void app_setup(void){
    const char *as = getenv("NFSU2_APPSUPPORT"), *home = getenv("HOME");
    if (as && *as) snprintf(g_support, sizeof g_support, "%s", as);
    else snprintf(g_support, sizeof g_support, "%s/Library/Application Support/NFSU2", home ? home : "/tmp");
    mkdirs(g_support);
    snprintf(g_settings, sizeof g_settings, "%s/settings.ini", g_support);
    app_load_settings();
    app_open_log();
    { time_t t = time(NULL); char ts[64]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
      fprintf(stderr, "=== NFSU2.app %s, %s ===\napp: support dir %s\n", ts, g_exe_dir, g_support); }
    if (g_set_logn) fwrite(g_set_log, 1, g_set_logn, stderr);
    app_set_defaults();
    char p[1200];
    if (!getenv("NFSU2_REGFILE")){
        snprintf(p, sizeof p, "%s/registry.txt", g_support);
        setenv("NFSU2_REGFILE", p, 1);
    }
    app_seed_registry(getenv("NFSU2_REGFILE"));
    if (!getenv("NFSU2_SAVEDIR")){
        snprintf(p, sizeof p, "%s/Overlay", g_support);
        setenv("NFSU2_SAVEDIR", p, 1);
    }
    /* Relative paths (frame dumps, debug outputs) must never resolve to
     * wherever Finder started us, let alone the game folder. */
    if (!getcwd(g_start_cwd, sizeof g_start_cwd)) g_start_cwd[0] = 0;
    if (chdir(g_support) != 0) fprintf(stderr, "app: chdir %s: %s\n", g_support, strerror(errno));
}

/* ---- the game folder ---------------------------------------------------- */

static int has_exe(const char *dir, char *out, size_t osz){
    char p[1200]; struct stat st;
    static const char *names[] = { "SPEED2.EXE", "speed2.exe", "Speed2.exe" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++){
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)){ snprintf(out, osz, "%s", p); return 1; }
    }
    return 0;
}

/* The chosen folder either contains SPEED2.EXE directly or is a parent
 * folder (such as "Need for Speed Underground 2 - Clean Edition").  We look
 * two levels down; anything deeper is worth asking the user about.      */
static int find_under(const char *dir, char *out, size_t osz, int depth){
    if (has_exe(dir, out, osz)) return 1;
    if (depth <= 0) return 0;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *kids = [fm contentsOfDirectoryAtPath:@(dir) error:nil];
    for (NSString *k in kids){
        if ([k hasPrefix:@"."]) continue;
        char sub[1200]; snprintf(sub, sizeof sub, "%s/%s", dir, [k UTF8String]);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode) &&
            find_under(sub, out, osz, depth - 1)) return 1;
    }
    return 0;
}

/* SPEED2.EXE must be the exact build the code was lifted from.  Returns 1
 * if it is; else fills why.  NFSU2_SKIP_EXE_CHECK=1 skips it (development). */
static int exe_ok(const char *exe, char *why, size_t wsz){
    const char *sk = getenv("NFSU2_SKIP_EXE_CHECK");
    if (sk && *sk && *sk != '0') return 1;
    int fd = open(exe, O_RDONLY);
    if (fd < 0){ snprintf(why, wsz, "%s cannot be read: %s", exe, strerror(errno)); return 0; }
    CC_SHA256_CTX ctx; CC_SHA256_Init(&ctx);
    static unsigned char buf[1 << 16]; ssize_t k; long long total = 0;
    while ((k = read(fd, buf, sizeof buf)) > 0){ CC_SHA256_Update(&ctx, buf, (CC_LONG)k); total += k; }
    close(fd);
    unsigned char md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(md, &ctx);
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2*i, 3, "%02x", md[i]);
    if (!strcmp(hex, EXE_SHA256)){
        fprintf(stderr, "app: %s is v1.2 NA (SHA-256 %.16s..., %lld bytes)\n", exe, hex, total);
        return 1; }
    snprintf(why, wsz,
             "This port only runs the North American v1.2 SPEED2.EXE "
             "(%d bytes, SHA-256 %.16s...).  The file in this folder is different "
             "(%lld bytes, SHA-256 %.16s...): the recompiled code is tied to that one "
             "exact build.  Choose a folder with the v1.2 executable.",
             EXE_SIZE, EXE_SHA256, total, hex);
    return 0;
}

/* Ask for the game folder.  0 = cancelled. */
static int app_ask(char *out, size_t osz, const char *msg){
    NSOpenPanel *op = [NSOpenPanel openPanel];
    [op setCanChooseFiles:NO];
    [op setCanChooseDirectories:YES];
    [op setAllowsMultipleSelection:NO];
    [op setPrompt:@"Use This Folder"];
    [op setMessage:@(msg)];
    [NSApp activateIgnoringOtherApps:YES];
    if ([op runModal] != NSModalResponseOK) return 0;
    snprintf(out, osz, "%s", [[[op URL] path] UTF8String]);
    return 1;
}

/* Find SPEED2.EXE: the command-line path (arg), else NFSU2_DATA
 * (environment or settings.ini game_folder), else ask with a folder picker
 * and remember the choice in settings.ini (remember: also for arg, i.e.
 * "NFSU2 --game-folder <dir>").  The folder must contain the v1.2 NA
 * executable.  NULL = give up.                                            */
const char *app_find_game(const char *arg, int remember){
    static char found[1200];
    char dir[1200], why[600], absd[1200]; why[0] = 0;
    const char *env = (arg && *arg) ? arg : getenv("NFSU2_DATA");
    if (env && *env && env[0] != '/' && g_start_cwd[0]){      /* relative to where we were started */
        snprintf(absd, sizeof absd, "%s/%s", g_start_cwd, env); env = absd; }
    if (env && *env){
        struct stat st;
        int ok = (stat(env, &st) == 0 && S_ISREG(st.st_mode))
                 ? (snprintf(found, sizeof found, "%s", env), 1)
                 : find_under(env, found, sizeof found, 2);
        if (!ok) snprintf(why, sizeof why, "There is no SPEED2.EXE in %s (the configured game folder).", env);
        else if (exe_ok(found, why, sizeof why)){
            if (remember){
                char keep[1200]; snprintf(keep, sizeof keep, "%s", found);
                char *s = strrchr(keep, '/'); if (s) *s = 0;
                settings_put("game_folder", keep);
            }
            return found;
        }
        fprintf(stderr, "app: %s\n", why);
    }
    if (no_ui()){
        fprintf(stderr, "app: no usable game folder and no UI (NFSU2_NO_UI/NOFOCUS): giving up\n");
        return NULL;
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    const char *msg = why[0] ? "Choose the Need for Speed Underground 2 folder (the one that contains SPEED2.EXE)."
                             : "Welcome!  Choose your Need for Speed Underground 2 folder (the one that "
                               "contains SPEED2.EXE, version 1.2).  It is only read, never changed.";
    if (why[0]) app_alert("The game folder cannot be used", why);
    for (int tries = 0; tries < 5; tries++){
        if (!app_ask(dir, sizeof dir, msg)) return NULL;
        if (!find_under(dir, found, sizeof found, 2)){
            app_alert("There is no SPEED2.EXE in this folder",
                      "Choose the folder the game is installed in: it contains SPEED2.EXE "
                      "and the CARS, TRACKS and SOUND folders.");
            continue; }
        if (!exe_ok(found, why, sizeof why)){ app_alert("Unsupported game version", why); continue; }
        char keep[1200]; snprintf(keep, sizeof keep, "%s", found);
        char *s = strrchr(keep, '/'); if (s) *s = 0;
        settings_put("game_folder", keep);
        return found;
    }
    return NULL;
}

/* When quitting without a game found, tell the user why. */
void app_no_game(void){
    app_alert("NFSU2 cannot start",
              "The game itself is not part of this app: it needs your own Need for Speed "
              "Underground 2 (v1.2) folder.  Open the app again to choose it, or set "
              "game_folder in ~/Library/Application Support/NFSU2/settings.ini.");
}
