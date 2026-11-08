/* Shared-engine browser port of NFSU2-SplitScreen.
 * Source: Copyright (c) 2026 mihaits, MIT (licenses/NFSU2-SplitScreen.txt).
 * Guest calls run through the lifted dispatcher, never native x86 pointers. */
#include "cpu.h"
#include "guest.h"
#include "game.h"
void garage_split_keep(CPU *c);            /* garage.c: slots 6-10 aside for the mode */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>
#include "splitscreen_layout.h"
#include "gamepad.h"
#include "kbd.h"
#include "coop.h"

/* the labels shown (split_label_text: in the game's language) */
enum { SL_SPLIT, SL_PLAYERS, SL_PLAYER, SL_PLAYER_UP, SL_JOIN, SL_JOIN_KB, SL_JOIN_SP, SL_N };
static void split_label_text(int which, unsigned n, char *out, size_t size);

/* more than one view drawn: a split race, or co-op free roam (coop.c: the career's game mode, player 2 on view 2,
 * which makes the view mode 3 by itself) */
int coop_in(void);
static int split_racing(void){
#ifdef __wasm__
    if (MR32(0x8707e0u) != 3) return 0;
    if (MR32(G_GAMEMODE) == 4) return MR32(0x89e7c0u) == 2;
    /* co-op: player 2 in free roam (coop.c's), or a career race the two race together (the game's two humans) */
    return MR32(G_GAMEMODE) == 1 && MR32(SPLIT_PLAYERS_COUNT) == 2 && (coop_in() || coop_racing_together());
#else
    return 0;
#endif
}
/* co-op: the HUDs' size in their halves (picked from 0.5, 0.6 and 0.7, 2026-11-04) */
static float split_coop_scale(void){ return 0.7f; }
/* co-op free roam's two views (coop.c): each player's free-roam HUD mapped into its half */
static int split_coop_hud(void){
#ifdef __wasm__
    /* (a career race together has the game's own 2P HUDs, made for halves) */
    return split_racing() && MR32(G_GAMEMODE) == 1 && !coop_racing_together();
#else
    return 0;
#endif
}
int split_render_limit(void){
    if (!split_racing()) return 1;
    unsigned n = MR32(SPLIT_PLAYERS_COUNT);
    return n >= 2 && n <= 6 ? (int)n : 2;
}
/* An address as the game's code names it -> where this build keeps it: the player tables 0x8900AC/B4/BC and the
 * input port tables 0x8655A0.. moved (splitscreen_mem.h), and test scripts name the game's addresses
 * (autotest.c peek/poke/waitmem/expect, roam.c's expressions).  The same address natively. */
uint32_t split_guest_addr(uint32_t a){
    if (a - 0x8900acu < 8u) return SPLIT_PLAYERS_INDEX + (a - 0x8900acu);
    if (a - 0x8900b4u < 8u) return SPLIT_PLAYERS_ORDER + (a - 0x8900b4u);
    if (a - 0x8900bcu < 4u) return SPLIT_PLAYERS_COUNT + (a - 0x8900bcu);
    if (a - 0x8655a0u < 0x390u) return SPLIT_PORT_TABLES + (a - 0x8655a0u);
    return a;
}
static uint32_t split_view(int id){ return 0x832de0u + (uint32_t)id * 0x70u; }
static int split_current_view;
static int split_streak_full_copy, split_streak_composite;
#define SPLIT_TARGETS 0x00d03000u
static SplitCell split_cell(int seat){
    unsigned width, height;
    void com_backbuffer_size(unsigned *, unsigned *);
    com_backbuffer_size(&width, &height);
    return split_layout_cell(split_render_limit(), seat, width, height, 0);
}

static int split_menu_players, split_entering, split_seat, split_carsel_active;
static uint32_t split_option, split_option_args[5];
static int split_count_dialog;
static int split_return_count;


static uint32_t split_cars[6], split_ports[6];
static uint8_t split_gears[6], split_picked[6];
static int split_joined[6], split_release_pad;
/* The keyboard takes up to two local seats: Enter one, Space another.  A lone keyboard player drives on the
 * whole keyboard (arrows or WASD); with two, Enter's player has the arrows' side and Space's WASD's.  Controller
 * ownership remains in connection/Start order; a keyboard seat does not consume a pad. */
enum { SPLIT_KB_ENTER, SPLIT_KB_SPACE };
static int split_kb_seat[2] = { -1, -1 };
static int split_kb_of(unsigned seat){
    for (int k = 0; k < 2; ++k) if (split_kb_seat[k] >= 0 && seat == (unsigned)split_kb_seat[k]) return k;
    return -1;
}
/* the controller of a seat that is not the keyboard's: the keyboard's seats before it skipped */
static unsigned split_pad_of(unsigned seat){
    unsigned pad = seat;
    for (int k = 0; k < 2; ++k) if (split_kb_seat[k] >= 0 && seat > (unsigned)split_kb_seat[k]) --pad;
    return pad;
}
void split_keyboard_device(PadState *pad){
    /* the game's pad device converts every seat's input: it must exist (split screen: a keyboard seat; co-op: player
     * 2's device, whose controller the page keeps from driving this pad) */
    int need = MR32(G_GAMEMODE) == 4 ? split_kb_seat[0] >= 0 || split_kb_seat[1] >= 0 : coop_device() >= 0;
    if (!pad->connected && need){
        memset(pad, 0, sizeof *pad);
        pad->connected = 1; pad->family = PADF_XBOX;
        strcpy(pad->name, "Split Screen keyboard");
    }
}
#ifdef __wasm__
/* A race's keys as a controller's buttons (bindings still run through the game's converter): nitrous (LB),
 * handbrake (RB), gear up (A), gear down (X), camera (Y), look back (B), up to two keys each (0 none), for the
 * whole keyboard, Enter's side (arrows) and Space's side (WASD).  No Right Alt: Windows' AltGr also sends Left
 * Ctrl. */
static const uint8_t SPLIT_KB_BUTTON[6] = { PAD_LB, PAD_RB, PAD_A, PAD_X, PAD_Y, PAD_B };
static const uint8_t SPLIT_KB_KEYS[3][6][2] = {
    { {0x2a, 0x36}, {0x39, 0}, {0x12, 0}, {0x10, 0}, {0x2e, 0}, {0x30, 0} },  /* Shift, Space, E, Q, C, B */
    { {0x36, 0}, {0x9d, 0x35}, {0x34, 0}, {0x33, 0}, {0x26, 0}, {0x27, 0} },  /* Right Shift, Right Ctrl or /, . , L ; */
    { {0x2a, 0}, {0x39, 0}, {0x12, 0}, {0x10, 0}, {0x2e, 0}, {0x30, 0} },     /* Left Shift, Space, E, Q, C, B */
};
static void split_kb_read(int side, int both, PadState *pad){
    uint8_t keys[256]; kbd_state(keys, sizeof keys);
    memset(pad, 0, sizeof *pad);
    pad->connected = 1; pad->family = PADF_XBOX;
    strcpy(pad->name, "Keyboard");
    /* driving: in the world (a split race: as the two-keyboard layout has it, its pause menu too); co-op: with no menu
     * or dialog up, where the arrows as a stick would be a second press of each (the pad's menu keystrokes,
     * dinput_pad.c) */
    int split = MR32(G_GAMEMODE) == 4;
    int racing = MR32(G_FLOW) == 6 && (split || coop_driving());
    /* START: Enter (only Enter's player's while two share the keyboard), and Space's player's Space outside a
     * split race (in one it is the handbrake; co-op's player 2 pauses with the keyboard's Esc) */
    if ((!both || side == SPLIT_KB_ENTER) && (keys[0x1c] || keys[0x9c])) pad->buttons |= 1u << PAD_START;
    if (side == SPLIT_KB_SPACE && split && !racing && keys[0x39]) pad->buttons |= 1u << PAD_START;
    /* Native keyboard messages handle car/menu navigation once, rather
     * than duplicating Enter and arrows through the virtual pad. */
    if (!racing) return;
    int arrows = !both || side == SPLIT_KB_ENTER, wasd = !both || side == SPLIT_KB_SPACE;
    int right = (arrows && keys[0xcd]) || (wasd && keys[0x20]), left = (arrows && keys[0xcb]) || (wasd && keys[0x1e]);
    pad->axis[PAD_AXIS_LX] = (float)(right - left);
    pad->axis[PAD_AXIS_RT] = (arrows && keys[0xc8]) || (wasd && keys[0x11]);
    pad->axis[PAD_AXIS_LT] = (arrows && keys[0xd0]) || (wasd && keys[0x1f]);
    const uint8_t (*map)[2] = SPLIT_KB_KEYS[both ? 1 + side : 0];
    for (unsigned b = 0; b < 6; ++b)
        if ((map[b][0] && keys[map[b][0]]) || (map[b][1] && keys[map[b][1]])) pad->buttons |= 1u << SPLIT_KB_BUTTON[b];
}
/* co-op free roam (coop.c, the career's game mode): seat 1 is player 2's device, a local pad's seat or the keyboard's
 * WASD half; seat 0 the arrows' half when player 2 has the WASD's (player 1's game input is the game's own, port 0) */
static int split_coop_seat(unsigned seat, PadState *pad){
    int dev = coop_device();
    memset(pad, 0, sizeof *pad);
    if (seat == 1 && dev == COOP_DEV_KB){ split_kb_read(SPLIT_KB_SPACE, 1, pad); return 1; }
    if (seat == 1 && dev >= 0) return gamepad_read_local((unsigned)dev, pad);
    if (seat == 0 && dev == COOP_DEV_KB){
        split_kb_read(SPLIT_KB_ENTER, 1, pad);
        pad->buttons &= ~(1u << PAD_START);     /* player 1's Enter stays the game's own (Engage, the menus) */
        return 1;
    }
    return 0;
}
/* the game's own keyboard while two share it in co-op: player 2's half is player 2's alone (dinput_kbd.c) */
int split_kb_hidden(unsigned dik){
    if (dik >= 256u || coop_device() != COOP_DEV_KB || !coop_driving()) return 0;   /* a menu has the whole keyboard */
    if (dik == 0x11 || dik == 0x1e || dik == 0x1f || dik == 0x20) return 1;          /* W A S D */
    for (unsigned b = 0; b < 6; ++b) if (SPLIT_KB_KEYS[2][b][0] == dik || SPLIT_KB_KEYS[2][b][1] == dik) return 1;
    return 0;
}
#endif
int split_read_local(unsigned seat, PadState *pad){
#ifdef __wasm__
    if (MR32(G_GAMEMODE) != 4 && coop_wanted()) return split_coop_seat(seat, pad);
    int side = split_kb_of(seat);
    if (side >= 0){ split_kb_read(side, split_kb_seat[0] >= 0 && split_kb_seat[1] >= 0, pad); return 1; }
    return gamepad_read_local(split_pad_of(seat), pad);
#else
    (void)seat; memset(pad, 0, sizeof *pad); return 0;
#endif
}
static uint32_t split_join_stamp;
static uint32_t split_join_counter(unsigned seat){
#ifdef __wasm__
    if (split_kb_of(seat) >= 0) return 0;
    return gamepad_local_join_stamp(split_pad_of(seat));
#else
    (void)seat; return 0;
#endif
}
static int split_requested(void){
#ifdef __wasm__
    /* on hot paths (the settings reads, 0x55DDA0's callers, every frame's hooks): the game mode first, and the
     * environment read once */
    if (MR32(G_GAMEMODE) != 4) return 0;
    static int env = -1;
    if (env < 0){ const char *s = getenv("NFSU2_SPLIT_PLAYERS"); env = s ? atoi(s) : 0; }
    int n = split_menu_players ? split_menu_players : env;
    if (n >= 2 && n <= 6){
        unsigned type = MR32(0x83aab4u);
        int max = type == 8 ? 2 : type == 2 ? 4 : 6;
        return n < max ? n : max;
    }
#endif
    return 0;
}
static int split_pause_port = -1, split_pause_seen, split_pause_released;
/* The browser exposes one virtual frontend device. Any local player can
 * lend it their own pad while they own the pause menu. */
void sub_0050d2d0_orig(CPU *c);
void sub_0050d2d0(CPU *c){
    if ((split_requested() || ((coop_in() || coop_racing_together()) && MR32(G_GAMEMODE) != 4)) && MR32(c->esp) == 0x4eb46eu){
        unsigned port = MR32(c->esp + 4);
        if (port < 6){
            split_pause_port = (int)port;
            split_pause_seen = split_pause_released = 0;
            MR32(c->esp + 4) = 0;
            fprintf(stderr, "[split] pause owner P%u, frontend slot 0\n", port + 1);
        }
    }
    sub_0050d2d0_orig(c);
}
static void split_pause_refresh(CPU *c){
    if (split_pause_port < 0) return;
    static const char *names[] = {"UI_Pause.fng", "UI_PauseOptions.fng",
        "UI_PauseOptionsMain.fng", "UI_PC_Customize_Options.fng"};
    int active = 0;
    for (unsigned i = 0; i < 4; ++i){
        uint32_t name = 0x00d09400u;
        strcpy((char *)g_mem + name, names[i]);
        active |= !!(gcall(c, 0x52cf60u, 0, 1, &name) & 255);
    }
    if (active) split_pause_seen = 1;
    else if (split_pause_seen || !(split_racing() || coop_in() || coop_racing_together())){
        fprintf(stderr, "[split] pause closed P%d\n", split_pause_port + 1);
        split_pause_port = -1;
    }
}
int split_menu_pad(PadState *pad){
#ifdef __wasm__
    if (split_carsel_active && MR32(G_GAMEMODE) == 4 && MR32(G_FLOW) != 6){
        if (!split_joined[split_seat]){
            if (split_seat) split_read_local((unsigned)split_seat-1, pad);
            pad->buttons &= 1u << PAD_B;
            memset(pad->axis, 0, sizeof pad->axis);
            return 1;
        }
        split_read_local((unsigned)split_seat, pad);
        if (!split_joined[split_seat] || split_release_pad){
            if (!pad->buttons) split_release_pad = 0;
            pad->buttons = 0; memset(pad->axis, 0, sizeof pad->axis);
        }
        return 1;
    }
    if (split_pause_port < 0 || !(split_requested() || ((coop_in() || coop_racing_together()) && MR32(G_GAMEMODE) != 4))) return 0;
    /* co-op: player 1's pause is the game's own (its controller already drives this pad) */
    if (!split_requested() && split_pause_port == 0) return 0;
    split_read_local((unsigned)split_pause_port, pad);
    if (!split_pause_released){
        if (!pad->buttons) split_pause_released = 1;
        else { pad->buttons = 0; memset(pad->axis, 0, sizeof pad->axis); }
    }
    return 1;
#else
    (void)pad;
    return 0;
#endif
}
#define SPLIT_SETTINGS 0x00d09000u
/* The original rumble engine owns only two channels. As in the ASI,
 * additional pairs temporarily borrow those channels with private state. */
static uint8_t split_ff_channels[2][0xc0], split_ff_strengths[2][0x60];
static void split_ff_swap(unsigned pair){
    for (unsigned i = 0; i < 0xc0; ++i){
        uint8_t t = MR8(0x8653b0u + i);
        MR8(0x8653b0u + i) = split_ff_channels[pair - 1][i];
        split_ff_channels[pair - 1][i] = t;
    }
    for (unsigned i = 0; i < 0x60; ++i){
        uint8_t t = MR8(0x864728u + i);
        MR8(0x864728u + i) = split_ff_strengths[pair - 1][i];
        split_ff_strengths[pair - 1][i] = t;
    }
}
void sub_005727b0_orig(CPU *c);
void sub_005727b0(CPU *c){
    unsigned port = MR32(c->esp + 8), stack = c->esp;
    if (!split_requested() || port < 2){ sub_005727b0_orig(c); return; }
    if (port >= (unsigned)split_requested()){ POP32(c->eip); return; }
    split_ff_swap(port / 2);
    MR32(stack + 8) = port & 1;
    sub_005727b0_orig(c);
    MR32(stack + 8) = port;
    split_ff_swap(port / 2);
}
void sub_00572610_orig(CPU *c);
void sub_00572610(CPU *c){
    CPU entry = *c;
    sub_00572610_orig(c);
    CPU result = *c;
    for (unsigned pair = 1; pair * 2 < (unsigned)split_requested(); ++pair){
        split_ff_swap(pair);
        *c = entry;
        sub_00572610_orig(c);
        split_ff_swap(pair);
    }
    *c = result;
}
void sub_00572e50_orig(CPU *c);
void sub_00572e50(CPU *c){
    if (split_requested() && MR32(c->esp + 4) >= 2){ POP32(c->eip); return; }
    sub_00572e50_orig(c);
}
void sub_005bfd70_orig(CPU *c);
void sub_005bfd70(CPU *c){
    if (MR32(c->esp + 4) >= 2){ c->eax = 0; POP32(c->eip); c->esp += 4; return; }
    sub_005bfd70_orig(c);
}
unsigned split_settings_address(unsigned original){
    return split_requested() ? SPLIT_SETTINGS + original - 0x83aa64u : original;
}
static void split_sync_settings(int new_race){
    static uint8_t seen[2][0x28];
    static int initialized;
    if (!split_requested()) return;
    for (unsigned k = 0; k < 2; ++k){
        uint8_t *mirror = g_mem + SPLIT_SETTINGS + k * 0x28, *game = g_mem + 0x83aa64u + k * 0x28;
        if (initialized && memcmp(mirror, seen[k], 0x28)) memcpy(game, mirror, 0x28);
        else memcpy(mirror, game, 0x28);
        memcpy(seen[k], mirror, 0x28);
    }
    if (new_race || !initialized) for (unsigned k = 2; k < 6; ++k)
        memcpy(g_mem + SPLIT_SETTINGS + k * 0x28, g_mem + SPLIT_SETTINGS + (k & 1) * 0x28, 0x28);
    if (new_race) for (unsigned k = 0; k < 6; ++k)
        if (split_picked[k]) MR8(SPLIT_SETTINGS + k * 0x28 + 0x15) = split_gears[k];
    initialized = 1;
}

void sub_00600090_orig(CPU *c);
void sub_00600090(CPU *c){
    uint32_t player = c->ecx, index = MR32(player + 0x10);
    if (!split_requested() || index < 2 || index >= 6){ sub_00600090_orig(c); return; }
    uint32_t twin = SPLIT_SETTINGS + (index & 1) * 0x28;
    uint8_t saved[0x28]; memcpy(saved, g_mem + twin, sizeof saved);
    memcpy(g_mem + twin, g_mem + SPLIT_SETTINGS + index * 0x28, sizeof saved);
    MR32(player + 0x10) = index & 1;
    sub_00600090_orig(c);
    MR32(player + 0x10) = index;
    memcpy(g_mem + twin, saved, sizeof saved);
}

void sub_00431f10_orig(CPU *c);
void sub_00431f10(CPU *c){
    uint32_t ctx = c->ecx;
    sub_00431f10_orig(c);
    if (split_requested() > 2 && (MR32(ctx) == 3 || MR32(ctx) > 4)){
        if (MR32(ctx) > 4){ MR32(ctx + 8) &= 15; MR32(ctx + 12) &= 15; }
        MR32(ctx) = 4;
    }
}
static unsigned split_racer_shift;
static void split_restore_seat(int seat);
static int split_hud_owner = -1;
static unsigned split_results_extra, split_results_count;
static void split_uncount_results(void){
    if (split_results_extra && MR32(0x89e7c8u) == split_results_count)
        MR32(0x89e7c8u) -= split_results_extra;
    split_results_extra = 0;
}
#define SPLIT_NAMES 0x00d04000u
#define SPLIT_HUD_NAMES 0x00d04100u
#define SPLIT_HUD_SOURCES 0x00d04300u
#define SPLIT_HUD_SCRATCH 0x00d04500u

void sub_00525ed0_orig(CPU *c);
void split_create_humans(CPU *c){
    split_uncount_results();
    uint32_t args[3] = {MR32(c->esp + 4), MR32(c->esp + 8), MR32(c->esp + 12)};
    sub_00525ed0_orig(c);
    int n = split_requested();
    if (args[0] != 2 || n <= 2) return;
    CPU saved = *c;
    for (unsigned shift = 2; shift < (unsigned)n; shift += 2){
        for (unsigned k = shift; k < shift + 2 && k < (unsigned)n; ++k)
            split_restore_seat((int)k);
        for (unsigned k = shift; k < shift + 2 && k < (unsigned)n; ++k)
            split_label_text(SL_PLAYER, k + 1, (char *)g_mem + SPLIT_NAMES + k * 16, 16);
        args[0] = (unsigned)n - shift < 2 ? (unsigned)n - shift : 2;
        c->esp = (saved.esp - 256u) & ~15u;
        for (unsigned k = 3; k; --k) PUSH32(args[k - 1]);
        PUSH32(0x525ed0u);
        split_racer_shift = shift;
        sub_00525ed0_orig(c);
        split_racer_shift = 0;
        *c = saved;
    }
    split_restore_seat(0); split_restore_seat(1);
    fprintf(stderr, "[split] created %d human racers; race entries=%u\n", n, MR32(0x89e814u));
}

void sub_006373e0_orig(CPU *c);
void sub_006373e0(CPU *c){
    if (split_racer_shift){
        uint32_t index = MR32(c->esp + 4) + split_racer_shift;
        MR32(c->esp + 4) = index;
        MR32(c->esp + 16) = SPLIT_NAMES + index * 16;
        uint32_t skin = index + 1;
        gcall(c, FN_RIDE_SKINS, MR32(c->esp + 12), 1, &skin);
    }
    sub_006373e0_orig(c);
}

static uint32_t split_alloc(CPU *c, uint32_t size){
    return gcall(c, FN_NEW, 0, 1, &size);
}

/* Native frontend flow. The extra option uses Quick Race's real vtable;
 * its identity selects the local setup, leaving Quick Race single-player. */
#define SPLIT_FE_TEXT 0x00d09800u
#define SPLIT_CHOOSE 0x53504c00u
static void split_text(uint32_t at, const char *s){ strcpy((char *)g_mem + at, s); }
void dispatch_patch(uint32_t, void (*)(CPU *));
void sub_004f98b0(CPU *c);
void sub_004fc050_orig(CPU *c);
static uint32_t split_main_menu;
static uint8_t split_main_carousel[0xe8];
static uint32_t split_count_options[3];
static uint32_t split_menu_buttons;
static unsigned split_menu_open_pending;

static int split_menu_escape;
/* Match IconScrollerMenu::React's native event/style sound lookup. */
static void split_menu_sound(CPU *c, unsigned event){
    uint32_t style=MR32(split_main_menu+0x30), manager=MR32(0x82b884u);
    int sound=(int8_t)MR8(0x837fe0u+event*0x1bu+style);
    if(manager && sound>=0){
        uint32_t a=(uint32_t)sound;
        gcall(c,0x458520u,manager,1,&a);
        if(getenv("NFSU2_SPLIT_MENU_LOG")) fprintf(stderr,"[split sound] event=%u sound=%d\n",event,sound);
    }
}
static void split_count_bar(CPU *c){
    uint32_t menu = split_main_menu, option = MR32(menu+0x58);
    uint32_t head=menu+0x50, count=0, selected=0;
    for (uint32_t node=MR32(head); node && node!=head && count<32; node=MR32(node)){
        ++count;
        if (node-4==option) selected=count;
    }
    if (selected && count>1){
        /* Use the active list for both the count selector and restored main
         * menu; their cached native scrollbar indices can be stale. */
        uint32_t a[] = {1, count, selected};
        gcall(c, 0x538080u, menu+0x84, 3, a);
        /* Main-menu animations leave the track and thumb fully transparent;
         * showing the objects alone does not restore their FE opacity. */
        for (unsigned j=0; j<2; ++j){
            uint32_t obj=MR32(menu+0xc4+j*4), data=obj?MR32(obj+0x2c):0;
            if (data){ MR32(data+12)=j ? 255 : 140; MR32(obj+0x1c)|=0x400000u; }
        }
    }
}
/* the main menu's heading as the game had it (its FEString's label hash, +0x60: the game's text in the player's
 * language), and whether the count dialog has run in this menu (from then on its scrollbar is ours to keep right) */
static uint32_t split_title_hash;
static int split_bar_own;
/* the game's text for a label hash (0x4FF9D0: edx the hash, the text in eax), 0 when it has none */
static uint32_t split_lang_text(CPU *c, uint32_t hash){
    CPU saved = *c;
    c->esp = (c->esp - 256u) & ~15u;
    c->edx = hash;
    PUSH32(0x4ff9d0u);
    dispatch_indirect(c, 0x4ff9d0u);
    uint32_t r = c->eax;
    *c = saved;
    return r && mem_is_committed(r) ? r : 0;
}
static void split_menu_title(CPU *c, const char *text){
    split_text(SPLIT_FE_TEXT+160, text);
    uint32_t a[] = {MR32(split_main_menu+4), 0x42adb44cu, SPLIT_FE_TEXT+160};
    gcall(c, 0x537b80u, 0, 3, a);
}
static void split_menu_fade_in(CPU *c){
    /* Play the native menu entrance once after the carousel is prepared. */
    split_text(SPLIT_FE_TEXT+320, "INIT");
    uint32_t a[] = {MR32(split_main_menu+4), 0x47ff4e7cu, SPLIT_FE_TEXT+320, 1};
    gcall(c, 0x537c00u, 0, 4, a);
    if (getenv("NFSU2_SPLIT_MENU_LOG")){
        uint32_t group=gcall(c,0x5379c0u,0,2,a), anim=group?MR32(group+0x54):0;
        fprintf(stderr,"[split fade] INIT active=%x\n",anim?MR32(anim+0x30):0);
    }
}
static uint32_t split_title_obj, split_logo_obj, split_title_align, split_logo_hidden;
static float split_title_geometry[4];
static uint32_t split_main_help_buttons;
/* UI_Main reserves 16 FE units for the logo above the carousel. Quick Race
 * does not. Keep native geometry for restoration, and remove that band only
 * while the player-count submenu owns these shared objects. */
static struct { uint32_t object, data; float y, height, target_y, target_height; } split_spacing[48];
static unsigned split_spacing_count;
static void split_spacing_keep(uint32_t o, float dy, float dh){
    if(!o || split_spacing_count>=48) return;
    for(unsigned i=0;i<split_spacing_count;i++) if(split_spacing[i].object==o) return;
    uint32_t d=MR32(o+0x2c); if(!d) return;
    unsigned i=split_spacing_count++;
    split_spacing[i].object=o; split_spacing[i].data=d;
    split_spacing[i].y=MRF32(d+0x20); split_spacing[i].height=MRF32(d+0x3c);
    split_spacing[i].target_y=split_spacing[i].y+dy;
    split_spacing[i].target_height=split_spacing[i].height+dh;
}
static void split_spacing_setup(CPU *c){
    split_spacing_count=0;
    uint32_t a[]={MR32(split_main_menu+4),0x8d7e0c53u};
    uint32_t root=gcall(c,0x5379c0u,0,2,a);
    for(uint32_t o=root?MR32(root+0x64):0,n=0;o && n<64;o=MR32(o+4),n++){
        uint32_t hash=MR32(o+0x10);
        if(hash==0x42adb44cu || hash==0x7609b60au) continue;
        if(hash==0x0de769b5u){
            for(uint32_t child=MR32(o+0x64),k=0;child && k<16;child=MR32(child+4),k++){
                uint32_t d=MR32(child+0x2c); float y=MRF32(d+0x20),h=MRF32(d+0x3c);
                if(y>50.f) split_spacing_keep(child,-16.f,0.f);
                else if(h>100.f) split_spacing_keep(child,-8.f,-16.f);
            }
        } else split_spacing_keep(o,-16.f,0.f);
    }
    const uint32_t hashes[]={0x444969fdu,0x444969feu,0xb04669e3u,0x3fbb7a4eu,0x688a1551u};
    for(unsigned i=0;i<sizeof hashes/sizeof hashes[0];i++){
        a[1]=hashes[i]; split_spacing_keep(gcall(c,0x5379c0u,0,2,a),-16.f,0.f);
    }
}
static void split_spacing_apply(int restore){
    for(unsigned i=0;i<split_spacing_count;i++){
        uint32_t d=split_spacing[i].data;
        MRF32(d+0x20)=restore?split_spacing[i].y:split_spacing[i].target_y;
        MRF32(d+0x3c)=restore?split_spacing[i].height:split_spacing[i].target_height;
        MR32(split_spacing[i].object+0x1c)|=0x400000u;
    }
    if(restore) split_spacing_count=0;
}
static void split_count_scheme(CPU *c, int save){
    if(save){
        split_spacing_setup(c);
        split_main_help_buttons=MR32(split_main_menu+0x20);
        MR32(split_main_menu+0x20)=0x404u;
        uint32_t hb=MR32(0x8384d8u), buttons[]={0x404u,MR32(split_main_menu+4)};
        if(hb) gcall(c,0x54e6e0u,hb,2,buttons);
        uint32_t a[]={MR32(split_main_menu+4),0x42adb44cu};
        split_title_obj=gcall(c,0x537a10u,0,2,a);
        a[1]=0x7609b60au;
        split_logo_obj=gcall(c,0x5379c0u,0,2,a);
        if(split_title_obj){
            uint32_t d=MR32(split_title_obj+0x2c);
            split_title_align=MR32(split_title_obj+0x6c);
            split_title_geometry[0]=MRF32(d+0x1c); split_title_geometry[1]=MRF32(d+0x20);
            split_title_geometry[2]=MRF32(d+0x38); split_title_geometry[3]=MRF32(d+0x3c);
        }
        if(split_logo_obj) split_logo_hidden=MR32(split_logo_obj+0x1c)&1u;
    }
    if(split_title_obj){
        uint32_t d=MR32(split_title_obj+0x2c);
        /* UI_QRModeSelect's heading: left aligned, 0.8 scale, above its bar. */
        MR32(split_title_obj+0x6c)=0;
        MRF32(d+0x1c)=-248.f; MRF32(d+0x20)=-63.f;
        MRF32(d+0x38)=MRF32(d+0x3c)=0.8f;
        MR32(split_title_obj+0x1c)|=0x400000u;
    }
    if(split_logo_obj) gcall(c,FN_FE_HIDE,0,1,&split_logo_obj);
    split_spacing_apply(0);
}
static void split_count_restore_scheme(CPU *c){
    split_spacing_apply(1);
    MR32(split_main_menu+0x20)=split_main_help_buttons;
    uint32_t hb=MR32(0x8384d8u), buttons[]={split_main_help_buttons,MR32(split_main_menu+4)};
    if(hb) gcall(c,0x54e6e0u,hb,2,buttons);
    if(split_title_obj){
        uint32_t d=MR32(split_title_obj+0x2c);
        MR32(split_title_obj+0x6c)=split_title_align;
        MRF32(d+0x1c)=split_title_geometry[0]; MRF32(d+0x20)=split_title_geometry[1];
        MRF32(d+0x38)=split_title_geometry[2]; MRF32(d+0x3c)=split_title_geometry[3];
        MR32(split_title_obj+0x1c)|=0x400000u;
    }
    if(split_logo_obj && !split_logo_hidden) gcall(c,0x50ca50u,0,1,&split_logo_obj);
    split_title_obj=split_logo_obj=0;
}
static void split_count_close(CPU *c){
    split_count_restore_scheme(c);
    uint32_t menu = split_main_menu;
    gcall(c, 0x51fd10u, menu+0x4c, 0, 0);
    memcpy(g_mem+menu+0x4c, split_main_carousel, sizeof split_main_carousel);
    /* The temporary carousel shares the FNG's image objects. Restore their
     * original texture hashes before the native carousel positions them. */
    uint32_t head=menu+0x50, node=MR32(head);
    for(unsigned guard=0;node && node!=head && guard<32;guard++,node=MR32(node)){
        uint32_t option=node-4, image=MR32(option+0x10);
        if(image){
            MR32(image+0x24)=MR32(option+0xc); MR32(image+0x1c)|=0x400000u;
            gcall(c,0x50ca50u,0,1,&image);
        }
    }
    split_count_dialog=0; split_menu_open_pending=0;
    gcall(c,0x538ca0u,menu,0,0);
    gcall(c,0x50da50u,menu,0,0);
    /* the game's own heading back, in the player's language */
    uint32_t text = split_title_hash ? split_lang_text(c, split_title_hash) : 0;
    if (text){ uint32_t a[] = {MR32(menu+4), 0x42adb44cu, text}; gcall(c, 0x537b80u, 0, 3, a); }
    else split_menu_title(c,"Main Menu");
    fprintf(stderr,"[split] main menu heading %08x back: %.40s\n", split_title_hash, text ? (char *)g_mem + text : "(English)");
    split_menu_fade_in(c);
    split_count_bar(c);
}
static void split_count_open(CPU *c, unsigned players){
    uint32_t menu=split_main_menu;
    if(!split_bar_own){
        uint32_t a[]={MR32(menu+4),0x42adb44cu}, title=gcall(c,0x537a10u,0,2,a);
        split_title_hash = title && mem_is_committed(title+0x63) ? MR32(title+0x60) : 0;
    }
    split_bar_own=1;
    memcpy(split_main_carousel,g_mem+menu+0x4c,sizeof split_main_carousel);
    gcall(c,0x51fd10u,menu+0x4c,0,0);
    /* Borrow the first three native icon objects and their spacing. Keeping
     * the existing widget preserves its panel, highlight and animations. */
    uint32_t source=MR32(menu+0x50), head=menu+0x50;
    for(unsigned k=0;k<3;k++){
        uint32_t p=split_count_options[k];
        if(!p) p=split_alloc(c,0x48);
        memcpy(g_mem+p,g_mem+source-4,0x48);
        source=MR32(source);
        MR32(p)=0x790c30u; MR32(p+0xc)=0x883e25d6u;
        MR32(p+0x2c)=SPLIT_CHOOSE+2+k; MR8(p+0x20)=MR8(p+0x22)=0;
        split_count_options[k]=p;
        uint32_t image=MR32(p+0x10);
        MR32(image+0x24)=0x883e25d6u; MR32(image+0x1c)|=0x400000u;
    }
    for(unsigned k=0;k<3;k++){
        uint32_t p=split_count_options[k];
        MR32(p+4)=k<2 ? split_count_options[k+1]+4 : head;
        MR32(p+8)=k ? split_count_options[k-1]+4 : head;
    }
    MR32(head)=split_count_options[0]+4; MR32(head+4)=split_count_options[2]+4;
    MR32(menu+0x58)=split_count_options[0]; MR32(menu+0x74)=3;
    MRF32(menu+0x4c+0xd8)-=16.f; /* native carousel icon baseline */
    uint32_t selected=players>=2 && players<=4 ? players-1 : 1;
    gcall(c,0x538a30u,menu+0x4c,1,&selected);
    gcall(c,0x538ca0u,menu,0,0);
    gcall(c,0x50da50u,menu,0,0);
    split_count_dialog=1;
    PadState pad; gamepad_read(&pad); split_menu_buttons=pad.buttons;
    uint8_t keys[256]; kbd_state(keys, sizeof keys); split_menu_escape=!!keys[1];
    { char s[96]; split_label_text(SL_SPLIT, 0, s, sizeof s); split_menu_title(c, s); }
    split_count_scheme(c,1);
    /* Hide unused shared icons before the first animated frame. */
    uint32_t node; memcpy(&node,split_main_carousel+4,4);
    for(unsigned k=0;node && node!=menu+0x50 && k<32;k++,node=MR32(node)){
        if(k>=3){ uint32_t image=MR32(node-4+0x10); if(image) gcall(c,FN_FE_HIDE,0,1,&image); }
    }
    split_menu_fade_in(c);
    split_count_bar(c);
    fprintf(stderr,"[split] player count carousel opened\n");
}
static void split_main_react(CPU *c){
    uint32_t msg=MR32(c->esp+4), menu=c->ecx;
    if(menu==split_main_menu){
        if(split_count_dialog && msg==0xc98356bau){
            PadState pad; gamepad_read(&pad);
            uint32_t edge=pad.buttons & ~split_menu_buttons;
            split_menu_buttons=pad.buttons;
            uint8_t keys[256]; kbd_state(keys, sizeof keys);
            int escape = keys[1] && !split_menu_escape;
            split_menu_escape = !!keys[1];
            if(escape || (edge & (1u<<PAD_B))){
                split_menu_sound(c,10); split_count_close(c); POP32(c->eip); c->esp+=16; return;
            }
        }
        if(split_count_dialog && msg==0x911ab364u){
            split_menu_sound(c,10); split_count_close(c); POP32(c->eip); c->esp+=16; return;
        }
        if(msg==0x0c407210u){
            uint32_t option=MR32(menu+0x58);
            if(!split_count_dialog && option==split_option){
                split_option_args[0]=MR32(menu+4); split_option_args[1]=msg;
                for(unsigned k=2;k<5;k++) split_option_args[k]=MR32(c->esp+k*4);
                split_menu_sound(c,8); split_menu_open_pending=2; POP32(c->eip); c->esp+=16; return;
            }
            if(split_count_dialog){
                for(unsigned k=0;k<3;k++) if(option==split_count_options[k]){
                    split_menu_sound(c,8);
                    split_menu_players=2+k;
                    split_count_close(c);
                    split_return_count=1;
                    split_entering=1;
                    gcall(c,0x4fc050u,split_option,5,split_option_args);
                    split_entering=0;
                    fprintf(stderr,"[split] menu selected %d players\n",split_menu_players);
                    break;
                }
                POP32(c->eip); c->esp+=16; return;
            }
        }
    }
    sub_004f98b0(c);
    /* Native Back unwinds Quick Race setup and restores its parent package.
     * Keep that cleanup, then restore the actual split-screen parent screen. */
    if(menu==split_main_menu && msg==0xc98356bau && split_return_count && !MR32(0x836340u)){
        split_return_count=0;
        split_count_open(c, split_menu_players);
    }
    /* Accept queues the native parent exit after React returns. Prepare the
     * replacement and start its entrance together on the following tick. */
    if(menu==split_main_menu && msg==0xc98356bau && split_menu_open_pending){
        unsigned players=split_menu_open_pending; split_menu_open_pending=0;
        split_count_open(c, players);
    }
    if(menu==split_main_menu && split_count_dialog) split_count_scheme(c,0);
    if(menu==split_main_menu && split_bar_own) split_count_bar(c);
}
/* ---- the labels in the game's language ---------------------------------------------------------------
 * The Latin-script languages: texts of ours, in each game's own words ("Splitscreen", "Ecran partagé" ...), into
 * the letters their fonts draw (language.c language_game_letters: Polish's, Turkish's, Hungarian's, Czech's,
 * Slovak's own).  The others' fonts spell in orders of their own, so theirs are cut from the game's own texts in
 * that language:
 * 0x6A2B93EB "2 Player Split-Screen" (the menu item's name, which ours replaces), 0xD9DC2F12 "PLAYER", 0xD87133BC
 * "Player 1", 0x1F181F57 "Press START" (0xEEEB248E "Press START button" where that is empty).  English where a cut
 * does not find what it expects (a language pack of another make). */
const char *language_name(void);
uint32_t language_text(uint32_t hash);
void language_game_letters(const char *utf8, char *out, size_t n);
/* a row: the language, then the texts in SL_* order */
static const struct { const char *lang, *split, *players, *player, *player_up, *join, *join_kb, *join_sp; } SPLIT_TEXTS[] = {
    { "english",    "Split Screen", "%u Players", "Player %u", "PLAYER %u",
                      "Player %u: press START", "Player %u: press START or Enter",
                      "Player %u: press START or Space" },
    { "french",     "Écran partagé", "%u joueurs", "Joueur %u", "JOUEUR %u",
                      "Joueur %u : appuyer sur START", "Joueur %u : appuyer sur START ou Entrée",
                      "Joueur %u : appuyer sur START ou Espace" },
    { "german",     "Splitscreen", "%u Spieler", "Spieler %u", "SPIELER %u",
                      "Spieler %u: START drücken", "Spieler %u: START oder Enter drücken",
                      "Spieler %u: START oder Leertaste drücken" },
    { "italian",    "Split-screen", "%u giocatori", "Giocatore %u", "GIOCATORE %u",
                      "Giocatore %u: premi START", "Giocatore %u: premi START o Invio",
                      "Giocatore %u: premi START o Spazio" },
    { "spanish",    "Pantalla dividida", "%u jugadores", "Jugador %u", "JUGADOR %u",
                      "Jugador %u: pulsa START", "Jugador %u: pulsa START o Intro",
                      "Jugador %u: pulsa START o Espacio" },
    { "dutch",      "Gesplitst scherm", "%u spelers", "Speler %u", "SPELER %u",
                      "Speler %u: druk op START", "Speler %u: druk op START of Enter",
                      "Speler %u: druk op START of de spatiebalk" },
    { "swedish",    "Delad skärm", "%u spelare", "Spelare %u", "SPELARE %u",
                      "Spelare %u: tryck på START", "Spelare %u: tryck på START eller Enter",
                      "Spelare %u: tryck på START eller mellanslag" },
    { "danish",     "Opdelt skærm", "%u spillere", "Spiller %u", "SPILLER %u",
                      "Spiller %u: tryk på START", "Spiller %u: tryk på START eller Enter",
                      "Spiller %u: tryk på START eller mellemrum" },
    { "portuguese", "Tela dividida", "%u jogadores", "Jogador %u", "JOGADOR %u",
                      "Jogador %u: pressione START", "Jogador %u: pressione START ou Enter",
                      "Jogador %u: pressione START ou Espaço" },
    { "polish",     "Podzielony ekran", "%u graczy", "Gracz %u", "GRACZ %u",
                      "Gracz %u: wciśnij START", "Gracz %u: wciśnij START lub Enter",
                      "Gracz %u: wciśnij START lub spację" },
    { "turkish",    "Bölünmüş Ekran", "%u Oyuncu", "Oyuncu %u", "OYUNCU %u",
                      "Oyuncu %u: START'a bas", "Oyuncu %u: START'a ya da Enter'a bas",
                      "Oyuncu %u: START'a ya da Boşluk tuşuna bas" },
    { "hungarian",  "Osztott képernyő", "%u játékos", "%u. Játékos", "%u. JÁTÉKOS",
                      "%u. Játékos: nyomj START gombot", "%u. Játékos: nyomj START vagy Enter gombot",
                      "%u. Játékos: nyomj START vagy Szóköz gombot" },
    { "czech",      "Rozdělená obrazovka", "%u hráči", "Hráč %u", "HRÁČ %u",
                      "Hráč %u: stiskni START", "Hráč %u: stiskni START nebo Enter",
                      "Hráč %u: stiskni START nebo mezerník" },
    { "slovak",     "Rozdelená obrazovka", "%u hráči", "Hráč %u", "HRÁČ %u",
                      "Hráč %u: stlač START", "Hráč %u: stlač START alebo Enter",
                      "Hráč %u: stlač START alebo medzerník" },
};
/* a text of the game's in its own letters (empty when it has none) */
static int split_game_text(uint32_t hash, char *out, size_t n){
    uint32_t s = language_text(hash);
    out[0] = 0;
    if (s && mem_is_committed(s)) snprintf(out, n, "%s", (const char *)g_mem + s);
    return out[0] != 0;
}
/* The cuts for a language whose fonts spell in orders of their own: the split-screen name, the players' word
 * around the number (pl_pre N pl_post), a player's (p_pre N p_post) and "Press START".  1 when the game's texts
 * are as the cut expects. */
typedef struct { char split[96], pl_pre[48], pl_post[48], p_pre[48], p_post[48], press[96]; } SplitCuts;
static int split_cuts(const char *lang, SplitCuts *k){
    char t[160], p[48], p1[48];
    memset(k, 0, sizeof *k);
    if (!split_game_text(0x6a2b93ebu, t, sizeof t) || !split_game_text(0xd9dc2f12u, p, sizeof p)) return 0;
    if (!strcmp(lang, "russian") || !strcmp(lang, "ukrainian")){      /* "<split>: 2 <players>" (2-4 the same) */
        char *c = strstr(t, ": 2 ");
        if (!c || c == t || !c[4]) return 0;
        snprintf(k->split, sizeof k->split, "%.*s", (int)(c - t), t);
        snprintf(k->pl_post, sizeof k->pl_post, " %s", c + 4);
    } else if (!strcmp(lang, "chinese")){                                 /* "2<player><split>" */
        size_t lp = strlen(p);
        if (t[0] != '2' || strncmp(t + 1, p, lp) || !t[1 + lp]) return 0;
        snprintf(k->split, sizeof k->split, "%s", t + 1 + lp);
        snprintf(k->pl_post, sizeof k->pl_post, "%s", p);
    } else if (!strcmp(lang, "korean")){         /* "2<people> <race mode>": "2-4<people> <race mode>" */
        char *sp = strchr(t, ' ');
        if (t[0] != '2' || !sp || sp == t + 1 || !sp[1]) return 0;
        snprintf(k->split, sizeof k->split, "2-4%s", t + 1);
        snprintf(k->pl_post, sizeof k->pl_post, "%.*s", (int)(sp - t - 1), t + 1);
    } else if (!strcmp(lang, "japanese")){
        /* "<versus mode>" (the game's own name of split screen, in all its split-screen titles); its "PLAYER" is
         * "driver settings", so the player's word is "Player 1" without its 1, a glyph of two bytes */
        size_t l1 = split_game_text(0xd87133bcu, p1, sizeof p1) ? strlen(p1) : 0;
        if (strchr(t, '2') || l1 < 4 || (uint8_t)p1[l1 - 2] < 0x80) return 0;
        snprintf(k->split, sizeof k->split, "%s", t);
        snprintf(k->p_pre, sizeof k->p_pre, "%.*s", (int)(l1 - 2), p1);
        snprintf(k->pl_post, sizeof k->pl_post, " %s", k->p_pre);
    } else if (!strcmp(lang, "thai")){       /* the run the mode select's and the car select's names share */
        char m[160], s[160];
        if (!split_game_text(0xef9f780du, m, sizeof m) || !split_game_text(0xf221d7edu, s, sizeof s)) return 0;
        size_t best = 0, at = 0, lm = strlen(m);
        for (size_t i = 0; i < lm; i++)
            for (size_t l = lm - i; l > best; l--){
                char w[160]; snprintf(w, sizeof w, "%.*s", (int)l, m + i);
                if (strstr(s, w)){ best = l; at = i; break; }
            }
        if (best < 3) return 0;
        snprintf(k->split, sizeof k->split, "%.*s", (int)best, m + at);
        snprintf(k->pl_post, sizeof k->pl_post, " %s", p);
    } else return 0;
    /* a player (unless the language's own cut made it): "Player 1" with its own 1 replaced, else "PLAYER N" */
    if (!k->p_pre[0]){
        size_t l1 = split_game_text(0xd87133bcu, p1, sizeof p1) ? strlen(p1) : 0;
        if (l1 > 1 && p1[l1 - 1] == '1' && !(p1[l1 - 2] >= '0' && p1[l1 - 2] <= '9'))
            snprintf(k->p_pre, sizeof k->p_pre, "%.*s", (int)(l1 - 1), p1);
        else snprintf(k->p_pre, sizeof k->p_pre, "%s ", p);
    }
    if (!split_game_text(0x1f181f57u, k->press, sizeof k->press) && !split_game_text(0xeeeb248eu, k->press, sizeof k->press))
        snprintf(k->press, sizeof k->press, "START");
    return 1;
}
/* label `which` (SL_*) for player or count n, in the game's letters */
static void split_label_text(int which, unsigned n, char *out, size_t size){
    const char *lang = language_name();
    unsigned e = 0;
    for (unsigned i = 0; i < sizeof SPLIT_TEXTS / sizeof SPLIT_TEXTS[0]; i++) if (!strcmp(lang, SPLIT_TEXTS[i].lang)) e = i + 1;
    SplitCuts k;
    if (!e && split_cuts(lang, &k)){
        switch (which){
        case SL_SPLIT: snprintf(out, size, "%s", k.split); break;
        case SL_PLAYERS: snprintf(out, size, "%s%u%s", k.pl_pre, n, k.pl_post); break;
        case SL_PLAYER: case SL_PLAYER_UP: snprintf(out, size, "%s%u%s", k.p_pre, n, k.p_post); break;
        default: snprintf(out, size, "%s%u%s: %s%s", k.p_pre, n, k.p_post, k.press,
                         which == SL_JOIN_KB ? " / Enter" : which == SL_JOIN_SP ? " / Space" : "");
        }
        return;
    }
    char u[160];
    const typeof(SPLIT_TEXTS[0]) *r = &SPLIT_TEXTS[e ? e - 1 : 0];
    const char *const f[SL_N] = { r->split, r->players, r->player, r->player_up, r->join, r->join_kb, r->join_sp };
    snprintf(u, sizeof u, f[which], n);
    language_game_letters(u, out, size);
}
void sub_004ff9d0_orig(CPU *c);
void sub_004ff9d0(CPU *c){
#ifdef __wasm__
    uint32_t h = c->edx;
    if (h == 0x6a2b93ebu || (h >= SPLIT_CHOOSE + 2 && h <= SPLIT_CHOOSE + 4)){
        uint32_t p = SPLIT_FE_TEXT + (h == 0x6a2b93ebu ? 0 : (h - SPLIT_CHOOSE) * 32);
        if (h == 0x6a2b93ebu) split_label_text(SL_SPLIT, 0, (char *)g_mem + p, 64);
        else split_label_text(SL_PLAYERS, h - SPLIT_CHOOSE, (char *)g_mem + p, 32);
        c->eax = p; POP32(c->eip); return;
    }
#endif
    sub_004ff9d0_orig(c);
}
static void split_add_menu(CPU *c, uint32_t menu){
    static int installed;
    if (!installed){ dispatch_patch(0x4f98b0u, split_main_react); installed = 1; }
    split_main_menu=menu; split_count_dialog=0; split_bar_own=0;
    uint32_t p = split_alloc(c, 0x48);
    if (!p) return;
    uint32_t a[] = {0x883e25d6u, 0x6a2b93ebu, 0};
    gcall(c, FN_ITEM, p, 3, a);
    MR32(p) = 0x790c30u;
    split_option = p;
    gcall(c, MR32(MR32(menu) + 0x18), menu, 1, &p);
    split_carsel_active = 0;
}
void sub_004fc050(CPU *c){ sub_004fc050_orig(c); }
void sub_0055b5c0_orig(CPU *c);
void sub_0055b5c0(CPU *c){ sub_0055b5c0_orig(c); }
static void split_save_seat(int seat){
    unsigned twin = seat & 1;
    split_cars[seat] = MR32(G_QR_CAR + twin * 4);
    split_ports[seat] = MR32(0x83a9d8u + twin * 4);
    split_gears[seat] = MR8(0x83aa79u + twin * 0x28);
    split_picked[seat] = 1;
    fprintf(stderr, "[split] player %d picked car %08x gearbox %u\n", seat+1, split_cars[seat], split_gears[seat]);
}
static void split_restore_seat(int seat){
    if (!split_picked[seat]) return;
    unsigned twin = seat & 1;
    MR32(G_QR_CAR + twin * 4) = split_cars[seat];
    MR32(0x83a9d8u + twin * 4) = split_ports[seat];
    MR8(0x83aa79u + twin * 0x28) = split_gears[seat];
}
static void split_label(CPU *c, const char *pkg, uint32_t hash, const char *text){
    split_text(SPLIT_FE_TEXT+256, pkg);
    uint32_t a[] = {SPLIT_FE_TEXT+256, hash};
    uint32_t obj = gcall(c, 0x537a10u, 0, 2, a);
    if (obj){
        split_text(SPLIT_FE_TEXT+320, text);
        uint32_t b[] = {obj, SPLIT_FE_TEXT+320};
        gcall(c, 0x537be0u, 0, 2, b);
    }
}
static int split_popup(CPU *c){
    split_text(SPLIT_FE_TEXT+256, "2P_PressStart.fng");
    uint32_t a = SPLIT_FE_TEXT+256;
    return gcall(c, 0x52cf60u, 0, 1, &a) & 255;
}
static void split_seat_title(CPU *c){
    char title[64];
    /* the keyboard's join key that is still free (this seat's own taking it does not count) */
    int enter = split_kb_seat[SPLIT_KB_ENTER] < 0 || split_kb_seat[SPLIT_KB_ENTER] == split_seat;
    int space = split_kb_seat[SPLIT_KB_SPACE] < 0 || split_kb_seat[SPLIT_KB_SPACE] == split_seat;
    split_label_text(enter ? SL_JOIN_KB : space ? SL_JOIN_SP : SL_JOIN, (unsigned)split_seat + 1, title, sizeof title);
    split_label(c, "2P_PressStart.fng", 0x2ebb974du, title);
    split_text(SPLIT_FE_TEXT+256, "Player_Name");
    uint32_t p = SPLIT_FE_TEXT+256;
    uint32_t hash = gcall(c, FN_FE_HASH, 0, 1, &p);
    split_label_text(SL_PLAYER_UP, (unsigned)split_seat + 1, title, sizeof title);
    split_label(c, "UI_QRCarSelect.fng", hash, title);
    split_label(c, "GarageMain.fng", hash, title);
    split_label(c, "UI_PC_Help_Bar.fng", hash, title);
}
void sub_004fa2a0_orig(CPU *c);
static void split_switch_seat(CPU *c, uint32_t carsel, int seat){
    split_seat = seat;
    split_carsel_active = 1;
    split_joined[seat] = 0;
    split_release_pad = 1;
    split_join_stamp = split_join_counter((unsigned)seat);
    uint32_t arg = seat & 1;
    /* Bypass the wrapper: this is our seat, not another native transition. */
    CPU saved = *c;
    c->esp = (c->esp - 256u) & ~15u;
    PUSH32(arg); PUSH32(0x4fa2a0u); c->ecx = carsel;
    sub_004fa2a0_orig(c); *c = saved;
    split_seat_title(c);
    fprintf(stderr, "[split] select car for player %d\n", seat+1);
}
void sub_0053fc20_orig(CPU *c);
static void split_start_selected(CPU *c){
    split_carsel_active = 0;
    split_restore_seat(0); split_restore_seat(1);
    gcall(c, 0x53fc20u, 0, 0, 0);
}
void sub_004fa2a0(CPU *c){
#ifdef __wasm__
    uint32_t ret = MR32(c->esp);
    if (split_requested()){
        if (ret == 0x4fc3adu || ret == 0x4fc9bbu){
            split_save_seat(split_seat);
            if (split_seat >= 2) split_restore_seat(0);
            if (split_seat + 1 >= split_requested()) split_start_selected(c);
            else split_switch_seat(c, c->ecx, split_seat+1);
            POP32(c->eip); c->esp += 4; return;
        }
        if (ret == 0x4fc728u){
            if (split_seat >= 2) split_restore_seat(1);
            split_switch_seat(c, c->ecx, split_seat > 0 ? split_seat-1 : 0);
            /* Back returns to an already joined player's car, without a
             * fresh native Press Start overlay. Keep that controller live. */
            split_joined[split_seat] = 1;
            POP32(c->eip); c->esp += 4; return;
        }
        { uint32_t s = MR32(c->esp+4); split_seat = s < 6 ? (int)s : 0; }   /* the game passes 0 or 1 */
        split_carsel_active = 1;
        split_joined[split_seat] = 0;
        split_release_pad = 1;
        split_join_stamp = split_join_counter((unsigned)split_seat);
    }
#endif
    sub_004fa2a0_orig(c);
}
void sub_0053fc20(CPU *c){
#ifdef __wasm__
    uint32_t ret = MR32(c->esp);
    if (split_requested() && (ret == 0x4fc3c3u || ret == 0x4fc9c7u)){
        split_save_seat(split_seat);
        if (split_seat >= 2) split_restore_seat(1);
        if (split_seat+1 < split_requested()){
            split_switch_seat(c, c->esi, split_seat+1);
            POP32(c->eip); return;
        }
        split_carsel_active = 0;
        split_restore_seat(0); split_restore_seat(1);
    }
#endif
    sub_0053fc20_orig(c);
}
static uint32_t split_world, split_cull_base;
static struct { uint32_t data; unsigned seat; int hide; } split_hud_data[8192];
static uint32_t split_minimap_hashes[140];
static void split_minimap_names(CPU *c){
    if (split_minimap_hashes[0]) return;
    static const char *fixed[] = {"Track2000_map", "Minimap_Mask", "Minimap_North_indicator", "TrackMapTargetRing"};
    uint32_t name = 0x00d09400u;
    for (unsigned i = 0; i < 140; ++i){
        if (i < 4) snprintf((char *)g_mem + name, 160, "%s", fixed[i]);
        else snprintf((char *)g_mem + name, 160, (char *)g_mem + MR32(0x7f6828u + ((i - 4) / 8) * 8), (i - 4) % 8);
        split_minimap_hashes[i] = gcall(c, FN_FE_HASH, 0, 1, &name);
    }
}
static unsigned split_hud_data_count, split_hud_copy;
void sub_004ca840_orig(CPU *c);
void sub_004ecb80_orig(CPU *c);
void sub_005370a0_orig(CPU *c);
#include "split_minimap.inc"
static void split_map_hud_objects(uint32_t obj, unsigned seat, unsigned depth, int hidden_group){
    if (depth > 32) return;
    for (unsigned guard = 0; obj && guard < 2048 && split_hud_data_count < 8192; ++guard, obj = MR32(obj + 4)){
        uint32_t data = MR32(obj + 0x2c);
        int hide = hidden_group;
        if (seat) for (unsigned i = 0; i < 140; ++i)
            if (MR32(obj + 0x10) == split_minimap_hashes[i]) hide = 1;
        MR32(obj + 0x1c) |= 0x400000u;
        if (data){
            split_hud_data[split_hud_data_count].data = data;
            split_hud_data[split_hud_data_count].hide = hide;
            split_hud_data[split_hud_data_count++].seat = seat;
        }
        if (MR32(obj + 0x18) == 5) split_map_hud_objects(MR32(obj + 0x64), seat, depth + 1, hide);
    }
}
static void split_map_huds(CPU *c){
    static unsigned frames;
    if (!split_racing() || (split_render_limit() <= 2 && !split_coop_hud())){ split_hud_data_count = 0; return; }
    if (++frames % 30 && split_hud_data_count) return;
    split_minimap_names(c);
    split_hud_data_count = 0;
    for (unsigned k = 0; k < (unsigned)split_render_limit(); ++k){
        uint32_t player = MR32(SPLIT_PLAYERS_INDEX + k * 4);
        uint32_t hud = player ? MR32(player + 0x9a4) : 0, name = hud ? MR32(hud + 0x10) : 0;
        if (!name) continue;
        uint32_t e = gcall(c, 0x51bb70u, 0x8637b0u, 1, &name), pkg = e ? MR32(e + 0x18) : 0;
        if (pkg) split_map_hud_objects(MR32(pkg + 0x4c), k, 0, 0);
    }
}
static void split_world_for(CPU *c, unsigned seat){
    if (!split_world) return;
    if (!seat) split_cull_base = MR32(split_world + 0x6c8);
    else if (split_cull_base) MR32(split_world + 0x6c8) = split_cull_base;
    if (seat < 2) return;
    uint32_t view = split_view(split_view_id(seat)), camera = MR32(view + 0x40);
    if (!camera) return;
    unsigned count = MR32(split_world + 0x6c0);
    for (unsigned i = 0; i < count && i < 12; ++i){
        uint32_t entry = split_world + i * 0x90, owner = MR32(entry + 0x40);
        if (!owner || MR32(owner + 4) < 5 || MR32(owner + 4) > 8) continue;
        if (owner == view) return;
        MR32(entry + 0x40) = view;
        uint32_t args[] = {view, entry};
        gcall(c, 0x5c26e0u, 0, 2, args);
        memcpy(g_mem + entry + 0x60, g_mem + camera + 0x40, 12);
        memcpy(g_mem + entry + 0x70, g_mem + camera + 0x50, 12);
        MR32(entry + 0x80) = MR32(view + 0xc);
        int mode = MR32(0x800df8u), section = -1;
        if (mode == 1 || (mode == 2 && MR32(0x7fb708u) % 0x3d >= 0xf)){
            int x = (int)MRF32(entry + 0x60), y = (int)MRF32(entry + 0x64);
            unsigned cell = (((y >> 5) & 0x7f) << 7) + ((x >> 5) & 0x7f);
            if (!(MR8(0x8832c0u + (cell >> 3)) & (1u << (cell & 7))))
                section = (((y >> 5) & 0x1f) << 5) + ((x >> 5) & 0x1f);
        }
        MR32(entry + 0x84) = section;
        MR32(entry + 0x48) = MR32(entry + 0x4c) = MR32(split_world + 0x6c8);
        MR32(entry + 0x50) = MR32(split_world + 0x6cc);
        gcall(c, 0x5e99c0u, split_world, 1, &entry);
        MR32(split_world + 0x6c8) = MR32(entry + 0x4c);
        return;
    }
}

#ifdef __wasm__
#include "split_reflections.inc"
static float split_blur_strength[6];
static uint32_t split_blur_saved;
static int split_blur_seat = -1;
static uint32_t split_blur_copy[4];
static int split_post_seat = -1;
#endif
int split_effect_region(uint32_t rect[2]){
#ifdef __wasm__
    if (split_post_seat >= 0 && split_racing()){
        unsigned w, h; void com_backbuffer_size(unsigned *, unsigned *);
        com_backbuffer_size(&w, &h);
        if (!w || !h) return 0;
        SplitCell cell = split_cell(split_post_seat);
        rect[0] = (uint32_t)lroundf(65535.f * cell.x / w) |
                  (uint32_t)lroundf(65535.f * cell.y / h) << 16;
        rect[1] = (uint32_t)lroundf(65535.f * (cell.x + cell.w) / w) |
                  (uint32_t)lroundf(65535.f * (cell.y + cell.h) / h) << 16;
        return 1;
    }
#endif
    return 0;
}
void sub_005bc180_orig(CPU *c);
void sub_005c5f70_orig(CPU *c);
void sub_005c6be0_orig(CPU *c);
void sub_0061f120_orig(CPU *c);
void sub_005c7ab0_orig(CPU *c);
void sub_006142c0_orig(CPU *c);
void sub_005d77f0_orig(CPU *c);
#include "split_post.inc"
/* StretchRect's rectangle here is physical, not the virtual menu rectangle. */
int split_blur_region(uint32_t dev, uint32_t src, uint32_t dst, uint32_t filter, int rect[4]){
#ifdef __wasm__
    if (split_blur_seat >= 0){
        SplitCell cell = split_cell(split_blur_seat);
        rect[0] = cell.x; rect[1] = cell.y;
        rect[2] = cell.x + cell.w; rect[3] = cell.y + cell.h;
        split_blur_copy[0] = dev; split_blur_copy[1] = src;
        split_blur_copy[2] = dst; split_blur_copy[3] = filter;
        return 1;
    }
#endif
    return 0;
}
uint32_t split_render_player(void){
    if (split_racing()) for (int k = 0; k < split_render_limit(); ++k)
        if (split_current_view == split_view_id(k)) return MR32(SPLIT_PLAYERS_INDEX + 4u*k);
    return 0;
}
void sub_005ba610_orig(CPU *c);
void sub_005ba610(CPU *c){
#ifdef __wasm__
    reflection_release_all(c);
    memset(split_blur_strength, 0, sizeof split_blur_strength);
    memset(split_ob_smooth, 0, sizeof split_ob_smooth);
    split_blur_seat = split_post_seat = split_rain_seat = -1;
#endif
    sub_005ba610_orig(c);
}

void sub_0060f880_orig(CPU *c);
void sub_0060f880(CPU *c){
    uint32_t view = MR32(c->esp + 4), id = view ? MR32(view + 4) : 0;
    int alias = split_racing() && id >= 5 && id <= 8;
    if (alias) MR32(view + 4) = 1;
    sub_0060f880_orig(c);
    if (alias) MR32(view + 4) = id;
}
static void split_init_view(CPU *c, unsigned seat){
    static uint32_t cameras[6];
    uint32_t view = split_view(split_view_id(seat));
    if (!MR32(view + 0x60)){
        uint32_t cull = split_alloc(c, 0x3b0), render = split_alloc(c, 0x51c0);
        if (!cull || !render) return;
        memset(g_mem + cull, 0, 0x3b0); memset(g_mem + render, 0, 0x51c0);
        gcall(c, 0x5e0260u, cull, 0, 0); MR32(cull) = view;
        uint32_t args[] = {view, 0};
        gcall(c, 0x61e150u, render, 2, args);
        MRF32(render + 0x509c) = 1.f;
        args[0] = 5; args[1] = 0x3f800000u;
        gcall(c, 0x613050u, render, 2, args);
        MR32(view + 0x60) = cull; MR32(view + 0x64) = render;
    }
    if (!cameras[seat]){
        cameras[seat] = split_alloc(c, 0x320);
        if (!cameras[seat]) return;
        memset(g_mem + cameras[seat], 0, 0x320);
        gcall(c, 0x44f5a0u, cameras[seat], 0, 0);
    }
    MR32(view + 0x40) = cameras[seat];
}

static uint32_t split_find_car(CPU *c, unsigned index){
    uint32_t end = gcall(c, 0x502fd0u, 0, 0, 0), list = MR32(G_WORLD);
    if (!list) return 0;
    uint32_t node = MR32(list);
    for (unsigned guard = 0; node && node != end && guard < 1024; ++guard, node = MR32(node))
        if (MR16(node + 0xa) == index) return node;
    return 0;
}

void sub_005f4250_orig(CPU *c);
void sub_005f4250(CPU *c){
    split_uncount_results();
    split_sync_settings(1);
    split_hud_data_count = 0;
    sub_005f4250_orig(c);
    int n = split_requested();
    /* the front end's player (0x85AD28) is the one who picked a car last; in the race, the results and the Post
     * Race menu it is player 1 again, as in single player: the questions there (Quit to Main Menu?) listen to it.
     * The car select is over: its last messages (while the race loaded) set split_carsel_active again, and it
     * would keep the front end's controller to B on the Split-Screen Mode Select the race goes back to. */
    if (n){ MR32(0x85ad28u) = 0; split_carsel_active = 0; }
    if (MR32(0x89e7c0u) != 2) return;
    for (unsigned index = 2; index < (unsigned)n; ++index){
        uint32_t racer = gcall(c, 0x5ed920u, G_RACEPARAMS, 1, &index);
        uint32_t car = split_find_car(c, index);
        if (!racer || MR32(racer + 4) != 1 || !car) continue;
        uint32_t player = split_alloc(c, 0x9c0);
        if (!player) continue;
        uint32_t twin = MR32(SPLIT_PLAYERS_INDEX + (index & 1) * 4);
        uint32_t args[] = {index, index & 1};
        split_hud_owner = index;
        gcall(c, 0x5f1670u, player, 2, args);
        split_hud_owner = -1;
        MR32(SPLIT_PLAYERS_INDEX + (index & 1) * 4) = twin;
        MR32(SPLIT_PLAYERS_INDEX + index * 4) = player;
        MR32(player + 0x10) = index;
        memcpy(g_mem + player + 0x30, g_mem + SPLIT_NAMES + index * 16, 16);
        split_init_view(c, index);
        MR32(player + 0x18) = split_view_id(index); MR32(player + 0x1c) = 0;
        gcall(c, 0x5f3e10u, player, 1, &car);
        fprintf(stderr, "[split] P%u player=%08x car=%08x view=%d\n", index + 1, player, car, split_view_id(index));
    }
}

void sub_005ed960_orig(CPU *c);
void sub_005ed920(CPU *c);
void sub_005ed960(CPU *c){
    if (split_requested() && MR32(c->esp + 4) >= 2) sub_005ed920(c);
    else sub_005ed960_orig(c);
}

static int split_clone_hud(CPU *c, uint32_t src, uint32_t dst){
    if (gcall(c, 0x51bb70u, 0x8637b0u, 1, &dst)) return 1;
    uint32_t entry = gcall(c, 0x51bb70u, 0x8637b0u, 1, &src);
    if (!entry) return 0;
    uint32_t hdr = MR32(entry + 0x10);
    if (!hdr || (MR32(hdr) != 0x30210 && MR32(hdr) != 0x30203)) return 0;
    int compressed = MR32(hdr) == 0x30210;
    uint32_t size = MR32(hdr + (compressed ? 20 : 4));
    if (!size || size > 16u * 1024 * 1024) return 0;
    uint32_t copy = split_alloc(c, size + 8);
    if (!copy) return 0;
    if (compressed){
        uint32_t args[] = {hdr + 12, copy + 8};
        gcall(c, 0x574670u, 0, 2, args);
    } else memcpy(g_mem + copy + 8, g_mem + hdr + 8, size);
    MR32(copy) = 0x30203; MR32(copy + 4) = size;
    const char *name = (const char *)g_mem + src, *dot = strrchr(name, '.');
    size_t len = dot ? (size_t)(dot - name) : strlen(name);
    if (!len) return 0;
    unsigned patched = 0;
    for (unsigned i = 8; i + len < size + 8 && i < 0x200; ++i)
        if (!strncasecmp((const char *)g_mem + copy + i, name, len)){
            MR8(copy + i + len - 1) = MR8(dst + len - 1); ++patched;
        }
    if (!patched) return 0;
    uint32_t ne = split_alloc(c, 0x54);
    if (!ne) return 0;
    gcall(c, 0x51b8e0u, ne, 1, &copy);
    uint32_t head = G_FEMGR, node = ne + 4, first = MR32(head);
    MR32(node) = first; MR32(node + 4) = head; MR32(first + 4) = node; MR32(head) = node;
    return 1;
}

void sub_004f8bf0_orig(CPU *c);
int coop_making_player(void);
void sub_004f8bf0(CPU *c){
    int who = split_hud_owner >= 2 ? split_hud_owner : (int)MR32(c->esp + 8);
    uint32_t fng = MR32(c->esp + 4);
    if (coop_making_player() && fng){
        /* co-op: player 2's HUD is free roam's own, a copy of the package under a name of its own (the Player ctor
         * named a 2P race HUD: it sees two local humans while it runs) */
        uint32_t src = SPLIT_HUD_SCRATCH, dst = SPLIT_HUD_NAMES + 64;
        snprintf((char *)g_mem + src, 64, "HUD_SingleRace.fng");
        snprintf((char *)g_mem + dst, 64, "HUD_SingleRac2.fng");
        if (split_clone_hud(c, src, dst)) MR32(c->esp + 4) = dst;
        else fprintf(stderr, "[coop] player 2's HUD: no copy of HUD_SingleRace (the 2P one stays)\n");
    }
    if (split_requested() > 2 && who >= 0 && who < 6 && fng && (who >= 2 || (who & 1))){
        uint32_t src = SPLIT_HUD_SCRATCH, dst = SPLIT_HUD_NAMES + who * 64;
        snprintf((char *)g_mem + src, 64, "%s", (char *)g_mem + fng);
        char *bot = strstr((char *)g_mem + src, "Bot");
        if (bot) memcpy(bot, "Top", 3);
        snprintf((char *)g_mem + dst, 64, "%s", (char *)g_mem + src);
        char *dot = strrchr((char *)g_mem + dst, '.');
        if (dot && dot > (char *)g_mem + dst){
            dot[-1] = '1' + who;
            if (split_clone_hud(c, src, dst)){
                snprintf((char *)g_mem + SPLIT_HUD_SOURCES + who * 64, 64, "%s", (char *)g_mem + fng);
                MR32(c->esp + 4) = dst;
            }
        }
        if (who >= 2) MR32(c->esp + 8) = who;
    }
    sub_004f8bf0_orig(c);
}

void sub_0055dda0_orig(CPU *c);
void sub_0055dda0(CPU *c){
    uint32_t msg = MR32(c->esp + 4), source = MR32(c->esp + 8);
    sub_0055dda0_orig(c);
    if (!split_requested() || !source) return;
    CPU saved = *c;
    for (unsigned k = 1; k < 6; ++k){
        uint32_t name = SPLIT_HUD_NAMES + k * 64, original = SPLIT_HUD_SOURCES + k * 64;
        if (!MR8(name) || strcasecmp((char *)g_mem + source, (char *)g_mem + original)) continue;
        c->esp = (saved.esp - 256u) & ~15u;
        PUSH32(name); PUSH32(msg); PUSH32(0x55dda0u);
        sub_0055dda0_orig(c);
        *c = saved;
    }
}

void sub_0056d850_orig(CPU *c);
void sub_0056d850(CPU *c){
    int n = split_requested(), missing = 0;
    if (n > 2){
        missing = (int)MR32(0x89e814u) - (int)MR32(0x89e7c0u) - (int)MR32(0x89e7c8u);
        if (missing < 0) missing = 0;
        if (missing > n - 2) missing = n - 2;
    }
    MR32(0x89e7c8u) += missing;
    sub_0056d850_orig(c);
    MR32(0x89e7c8u) -= missing;
}

void sub_0054cca0_orig(CPU *c);
void sub_0054cca0(CPU *c){
    split_uncount_results();
    int n = split_requested();
    if (n > 2){
        int counted = (int)MR32(0x89e7c0u) + (int)MR32(0x89e7c4u) + (int)MR32(0x89e7c8u) + (int)MR32(0x89e7ccu);
        int missing = (int)MR32(0x89e814u) - counted;
        if (missing < 0) missing = 0;
        if (missing > n - 2) missing = n - 2;
        MR32(0x89e7c8u) += missing;
        split_results_extra = missing; split_results_count = MR32(0x89e7c8u);
    }
    sub_0054cca0_orig(c);
}

#define SPLIT_STREAM_POS 0x00d05000u
#define SPLIT_STREAM_LIST 0x00d05200u
#define SPLIT_POOL_RECORDS 0x00d06000u
static int16_t split_zones[4], split_used_zones[4];
void sub_005e5300_orig(CPU *c);
void sub_005e5300(CPU *c){
    sub_005e5300_orig(c);
    for (unsigned k = 0; k < 4; ++k){
        split_zones[k] = 0;
        if (split_racing() && k + 2 < (unsigned)split_render_limit()){
            uint32_t view = split_view(split_view_id(k + 2)), camera = MR32(view + 0x40);
            if (MR8(view + 8) && camera){
                uint32_t pos = SPLIT_STREAM_POS + k * 0x5c;
                MR32(pos) = 0xffffffffu;
                memcpy(g_mem + pos + 4, g_mem + camera + 0x40, 8);
                memcpy(g_mem + pos + 12, g_mem + camera + 0x1e0, 8);
                split_zones[k] = (int16_t)gcall(c, 0x5e4f20u, 0x88eb00u, 1, &pos);
            }
        }
        if (split_zones[k] != split_used_zones[k]) c->eax = (c->eax & ~255u) | 1;
    }
}
static unsigned split_zone_sections(CPU *c, int16_t zone){
    uint32_t head = 0x883e90u, node = MR32(head);
    for (unsigned guard = 0; node && node != head && guard < 4096; ++guard, node = MR32(node)){
        int count = (int16_t)MR16(node + 0x18);
        for (int i = 0; i < count; ++i) if ((int16_t)MR16(node + 0x1a + i * 2) == zone){
            uint32_t args[] = {node, SPLIT_STREAM_LIST, 0x180};
            unsigned n = gcall(c, 0x5dff50u, 0x883e70u, 3, args);
            return n <= 0x180 ? n : 0;
        }
    }
    head = 0x883e80u; node = MR32(head);
    for (unsigned guard = 0; node && node != head && guard < 4096; ++guard, node = MR32(node)){
        if ((int16_t)MR16(node + 8) != zone) continue;
        int count = (int)MR32(node + 0x10);
        if (count <= 0) return 0;
        if (count > 0x180) count = 0x180;
        memcpy(g_mem + SPLIT_STREAM_LIST, g_mem + node + 0x14, count * 2);
        return count;
    }
    return 0;
}
void sub_005e5110_orig(CPU *c);
void sub_005e5110(CPU *c){
    uint32_t streamer = c->ecx;
    sub_005e5110_orig(c);
    for (unsigned k = 0; k < 4; ++k){
        split_used_zones[k] = split_zones[k];
        if (split_zones[k] <= 0) continue;
        unsigned n = split_zone_sections(c, split_zones[k]);
        if (n){
            uint32_t args[] = {SPLIT_STREAM_LIST, n, k & 1};
            gcall(c, 0x5dacb0u, streamer, 3, args);
        }
    }
}
void sub_005df580_orig(CPU *c);
void sub_005df580(CPU *c){
    uint32_t streamer = c->ecx;
    sub_005df580_orig(c);
    if (!split_racing() || split_render_limit() <= 2) return;
    unsigned count = MR32(streamer + 0x298);
    for (unsigned i = 0; i < count && i < 256; ++i){
        uint32_t section = MR32(streamer + 0x29c + i * 4);
        if (!section) continue;
        int best = 0x63;
        for (unsigned k = 0; k < 6; ++k){
            if (k >= 2 && (k >= (unsigned)split_render_limit() || split_zones[k - 2] <= 0)) continue;
            if (!(MR8(section + 0xb) & (1u << (k & 1)))) continue;
            uint32_t args[] = {section, k < 2 ? streamer + 0xb0 + k * 0x5c : SPLIT_STREAM_POS + (k - 2) * 0x5c};
            int distance = (int)gcall(c, 0x5df3a0u, streamer, 2, args);
            if (distance < best) best = distance;
        }
        MR32(section + 0x40) = best * 100000 + MR32(section + 0x20);
    }
}

void sub_005e4e70_orig(CPU *c);
void coop_note_pools(int big);
void sub_005e4e70(CPU *c){
    uint32_t streamer = c->ecx;
    int n = split_requested();
    /* co-op free roam (coop.c): two players' areas of the city, as two players' split race (the 1.5x); a career race
     * the two race together too (two views; the game's own two-human pool is smaller than one human's) */
    int coop = MR32(G_GAMEMODE) == 1 && ((MR32(G_RACETYPE) == 0 && coop_wanted()) || coop_racing_together());
    if (coop) n = 2;
    coop_note_pools(coop);
    if (n && (MR32(0x89e7c0u) == 2 || coop)){
        uint64_t bytes = (uint64_t)MR32(0x79dc54u) * 512 * (n + 1);
        if (bytes > 256u * 1024 * 1024) bytes = 256u * 1024 * 1024;
        if (bytes > MR32(c->esp + 4)) MR32(c->esp + 4) = (uint32_t)bytes;
    } else n = 0;
    sub_005e4e70_orig(c);
    uint32_t pool = MR32(streamer + 0x6a4);
    if (n && pool){
        uint32_t list = pool + 12;
        for (unsigned k = 0; k < 320; ++k){
            uint32_t record = SPLIT_POOL_RECORDS + k * 20;
            memset(g_mem + record, 0, 20);
            MR32(record + 4) = MR32(list + 4); MR32(record) = list;
            MR32(MR32(list + 4)) = record; MR32(list + 4) = record;
        }
    }
}

void sub_00610750_orig(CPU *c);
void split_second_pool(CPU *c){
    int n = split_requested();
    /* co-op free roam: room for player 2's car (another of player 1's) beside the game's 11000 KB; online free
     * roam's own size (roam_game.c, 48000 KB and up) has it already */
    if (MR32(G_GAMEMODE) == 1 && MR32(G_RACETYPE) == 0 && coop_wanted() && MR32(c->esp + 4) < 17000u * 1024u)
        MR32(c->esp + 4) = 17000u * 1024u;
    if (n && MR32(0x89e7c0u) == 2){
        uint64_t bytes = (uint64_t)MR32(0x7fa9d4u) * 512 * n;
        if (bytes > 256u * 1024 * 1024) bytes = 256u * 1024 * 1024;
        if (bytes > MR32(c->esp + 4)) MR32(c->esp + 4) = (uint32_t)bytes;
    }
    sub_00610750_orig(c);
}

void sub_00579550_orig(CPU *c);
void sub_00579550(CPU *c){
    sub_00579550_orig(c);
#ifdef __wasm__
    /* The original initializer covers ports 0/1 only. The front-end action
     * handler has FOUR slots: adding it for ports 4/5 corrupts adjacent state.
     * Those ports get only their id; race-player bindings are added later. */
    for (uint32_t port = 2; port < SPLIT_PORT_CAPACITY; ++port){
        MR32(SPLIT_PORT_TABLES + port * 0x1c8u + 0x1b4u) = port;
        if (port < 4){
            const uint32_t args[] = {port, 1, 0x00571a20u, 0};
            gcall(c, 0x00579410u, 0, 4, args);
        }
    }
#endif
}

#ifdef __wasm__
#define SPLIT_INPUT_CURRENT 0x00d01000u
#define SPLIT_INPUT_PREVIOUS 0x00d01100u
#define SPLIT_INPUT_RECORD 0x00d02000u
int dipad_local_state(unsigned seat, uint8_t out[0x110]);
static uint32_t split_pad_record(void){
    for (unsigned i = 0; i < 6; ++i){
        uint32_t p = 0x874c40u + i * 0x370u;
        if (MR32(p + 0x230u) == 0 && MR32(p + 0x234u) != 0x13u && MR32(p + 0x234u)) return p;
    }
    return 0;
}
static void split_poll(CPU *c){
    uint32_t record = split_pad_record(), copy = MR32(0x870760u);
    for (unsigned seat = 0; seat < 6; ++seat){
        uint32_t cur = SPLIT_INPUT_CURRENT + seat * 16u;
        memcpy(g_mem + SPLIT_INPUT_PREVIOUS + seat * 16u, g_mem + cur, 16);
        MR32(cur) = MR32(cur + 4) = 0xffffffffu;
        MR32(cur + 8) = 0x7f800000u;
        MR32(cur + 12) = MR32(0x86eafcu);
        if (!record || !copy || !MR32(0x870988u)) continue;
        uint8_t state[0x110], saved_live[0x110], saved_copy[0x110];
        if (!dipad_local_state(seat, state)) continue;
        memcpy(g_mem + SPLIT_INPUT_RECORD, g_mem + record, 0x370);
        MR32(SPLIT_INPUT_RECORD + 0x33cu) = 0; /* never capture a binding */
        memcpy(saved_live, g_mem + 0x86ffc0u, sizeof saved_live);
        memcpy(saved_copy, g_mem + copy, sizeof saved_copy);
        memcpy(g_mem + 0x86ffc0u, state, sizeof state);
        memcpy(g_mem + copy, state, sizeof state);
        gcall(c, 0x5c1840u, SPLIT_INPUT_RECORD, 0, 0);
        memcpy(g_mem + 0x86ffc0u, saved_live, sizeof saved_live);
        memcpy(g_mem + copy, saved_copy, sizeof saved_copy);
        MR32(cur) = MR32(SPLIT_INPUT_RECORD + 8);
        MR32(cur + 4) = MR32(SPLIT_INPUT_RECORD + 12);
        MR32(cur + 8) = MR32(SPLIT_INPUT_RECORD + 16);
    }
}
#endif

/* one input record into another (16 bytes: two words of actions, a bit 0 while pressed; four bytes of axes, the
 * first two from 0, the others centred on 0x80; the time): pressed in either, each axis the further one */
static void split_merge_input(uint32_t into, uint32_t from){
    MR32(into) &= MR32(from); MR32(into + 4) &= MR32(from + 4);
    for (unsigned k = 0; k < 4; ++k){
        int a = MR8(into + 8 + k), b = MR8(from + 8 + k), mid = k < 2 ? 0 : 0x80;
        if (abs(b - mid) > abs(a - mid)) MR8(into + 8 + k) = (uint8_t)b;
    }
}
void sub_005cfd60_orig(CPU *c);
void sub_005cfd60(CPU *c){
#ifdef __wasm__
    uint32_t port = MR32(c->esp + 4u), type = MR32(c->esp + 8u), previous = MR32(c->esp + 12u);
    if (MR32(G_GAMEMODE) != 4 && (coop_in() || coop_racing_together()) && MR32(G_FLOW) == 6 && port < 2){
        /* co-op free roam: port 0 stays the game's own (player 1's keys and controller); port 1 is player 2's seat.
         * With the keyboard shared, player 1's arrows' half joins port 0's own (player 2's keys are hidden from it) */
        static uint32_t port_type;
        if (!port){
            split_pause_refresh(c);
            sub_005cfd60_orig(c);
            uint32_t native = c->eax;
            port_type = type ? MR32(type) : 0;
            split_poll(c);
            /* driving only: a menu reads port 0 as well (an arrow would count twice) */
            if (coop_device() == COOP_DEV_KB && native && coop_driving()) split_merge_input(native, SPLIT_INPUT_CURRENT);
            /* a menu or dialog up: player 2's port at rest (the menus read the ports too: a gas held when the pause
             * menu came up moved it) */
            if (!coop_driving()){
                uint32_t p2 = SPLIT_INPUT_CURRENT + 16u;
                MR32(p2) = MR32(p2 + 4) = 0xffffffffu;
                MR8(p2 + 8) = MR8(p2 + 9) = 0; MR8(p2 + 10) = MR8(p2 + 11) = 0x80;
            }
            return;
        }
        POP32(c->eip);
        if (type) MR32(type) = port_type;
        if (previous) MR32(previous) = SPLIT_INPUT_PREVIOUS + port * 16u;
        c->eax = SPLIT_INPUT_CURRENT + port * 16u;
        return;
    }
    if (MR32(G_GAMEMODE) == 4 && MR32(G_FLOW) == 6 && port < 6){
        static uint32_t port_type;
        if (!port){
            split_pause_refresh(c);
            sub_005cfd60_orig(c); /* polls real devices and frontend input once */
            port_type = type ? MR32(type) : 0;
            split_poll(c);
            /* While paused the original frontend device already contains
             * the owner's routed pad. Do not replace it with player one's
             * racing buffer, which would lock out owners on ports 2..5. */
            if (split_pause_port >= 0) return;
        } else POP32(c->eip);
        if (type) MR32(type) = port_type;
        if (previous) MR32(previous) = SPLIT_INPUT_PREVIOUS + port * 16u;
        c->eax = SPLIT_INPUT_CURRENT + port * 16u;
        return;
    }
#endif
    sub_005cfd60_orig(c);
}

void sub_005c21e0_orig(CPU *c);
void sub_005c21e0(CPU *c){
    sub_005c21e0_orig(c);
#ifdef __wasm__
    /* the reflection view's target and views 4 and 9 as the game had them before a split race, put back after it
     * (single player's road reflection keeps its own size and settings) */
    static int kept; static uint32_t kept_rt, kept_size[3]; static uint8_t kept_on[2];
    if (split_racing() && !kept){
        uint32_t rt = MR32(split_view(4) + 0x58);
        kept_rt = rt;
        if (rt) for (int i = 0; i < 3; ++i) kept_size[i] = MR32(rt + 16 + 4 * i);
        kept_on[0] = MR8(split_view(4) + 8); kept_on[1] = MR8(split_view(9) + 8);
        kept = 1;
    } else if (!split_racing() && kept){
        uint32_t rt = MR32(split_view(4) + 0x58);
        if (rt && rt == kept_rt && MR32(rt + 16) == 1 && MR32(rt + 20) == 320 && MR32(rt + 24) == 240)
            for (int i = 0; i < 3; ++i) MR32(rt + 16 + 4 * i) = kept_size[i];
        MR8(split_view(4) + 8) = kept_on[0]; MR8(split_view(9) + 8) = kept_on[1];
        kept = 0;
    }
    if (split_racing()){
        for (int seat = 0; seat < split_render_limit(); ++seat){
            uint32_t view = split_view(split_view_id(seat)), rt = SPLIT_TARGETS + seat * 28u;
            SplitCell cell = split_cell(seat);
            memcpy(g_mem + rt, g_mem + 0x86e768u, 28);
            MR32(rt + 4) = split_view_id(seat); MR32(rt + 16) = 1;
            MR32(rt + 20) = cell.w; MR32(rt + 24) = cell.h;
            MR32(view + 0x58) = rt; MR8(view + 8) = 1;
        }
        uint32_t reflection = split_view(4), rt = MR32(reflection + 0x58);
        if (rt){ MR32(rt + 16) = 1; MR32(rt + 20) = 320; MR32(rt + 24) = 240; }
        MR8(reflection + 8) = 1; MR8(split_view(9) + 8) = 1;
    } else {
        for (int seat = 0; seat < 6; ++seat){
            uint32_t view = split_view(split_view_id(seat));
            if (MR32(view + 0x58) == SPLIT_TARGETS + seat * 28u){
                MR32(view + 0x58) = seat ? 0 : 0x86e768u;
                if (seat) MR8(view + 8) = 0;
            }
        }
    }
#endif
}

/* HOOK_SITE(0x004aeed8) HOOK_SITE(0x004fc06b) HOOK_SITE(0x005cb004) HOOK_SITE(0x005cb72d)
 * HOOK_SITE(0x005c80b0) HOOK_SITE(0x005dacdb)
 * HOOK_SITE(0x005cb31f) HOOK_SITE(0x005cb324)
 * HOOK_SITE(0x005cc474) HOOK_SITE(0x005cc490)
 * HOOK_SITE(0x005c7ab8) HOOK_SITE(0x005c7c04) HOOK_SITE(0x005c7c47) HOOK_SITE(0x005c7dd4)
 * HOOK_SITE(0x006136c7) HOOK_SITE(0x00614382) HOOK_SITE(0x0061e73a)
 * HOOK_SITE(0x005d683c) HOOK_SITE(0x005d686f)
 * HOOK_SITE(0x005dbe91) HOOK_SITE(0x005dbeef) HOOK_SITE(0x005dbf49)
 * HOOK_SITE(0x004ca902) HOOK_SITE(0x004ac6d9) HOOK_SITE(0x004cad01)
 * HOOK_SITE(0x005cbc02) HOOK_SITE(0x005cbda6) HOOK_SITE(0x005cbdab)
 * HOOK_SITE(0x005c6ec6) HOOK_SITE(0x005cbbb0) HOOK_SITE(0x005cbbb5)
 * HOOK_SITE(0x005caccc) HOOK_SITE(0x005cae12) HOOK_SITE(0x006312b0)
 * HOOK_SITE(0x00498070) HOOK_SITE(0x004fc1f0) HOOK_SITE(0x005b9caa)
 * HOOK_SITE(0x0051b0b0) HOOK_SITE(0x00606fe8) HOOK_SITE(0x005f6771)
 * HOOK_SITE(0x005f678c) HOOK_SITE(0x004174c5) */
int splitscreen_hook(CPU *c, uint32_t ip){
#ifdef __wasm__
    if (ip == 0x5cacccu){ reflection_roads(c); return 1; }
    if (ip == 0x4ca902u){ if (mm_center >= 0) MR32(c->esp) = mm_center; return 1; }
    if (ip == 0x4ac6d9u){ MR32(c->esp) = 128; return 1; }
    if (ip == 0x4cad01u){
        /* Keep distant local racers on the native map's clamped edge. */
        uint32_t e = c->edi, desc = MR32(e + 0x1c);
        if (mm_center >= 0 && desc && MR32(desc+4) == 4 && MR32(e+8) < (uint32_t)split_render_limit()){
            MRF32(c->esp+0x38) = 1.e20f;
            MR32(e+0x18) &= ~3u;
        }
        return 1;
    }
    if (ip == 0x51b0b0u && split_racing() && mm_transform(c)) return 1;
    if (ip == 0x5d683cu || ip == 0x5d686fu){
        uint32_t ctx = MR32(c->ebp + 12), view = ctx ? MR32(ctx + 0x40) : 0;
        if (split_racing() && view && MR32(view + 4) >= 5 && MR32(view + 4) <= 8)
            MR32(c->esp) = MR32(view + 4);
        return 1;
    }
    if (ip == 0x5dbe91u){
        if (split_racing() && c->ebx >= 1 && c->ebx <= 6) MR32(c->esp) = split_view_id(c->ebx - 1);
        return 1;
    }
    if (ip == 0x5dbeefu){
        if (split_racing() && c->ebx >= 1 && c->ebx <= 6) c->eax = MR32(SPLIT_PLAYERS_ORDER + 4u*(c->ebx - 1));
        return 1;
    }
    if (ip == 0x5dbf49u){
        if (split_racing()){ c->zf = c->ebx <= (unsigned)split_render_limit(); c->sf = c->of = 0; }
        return 1;
    }
    if (ip == 0x5c7ab8u || ip == 0x5c7c04u || ip == 0x5c7c47u || ip == 0x5c7dd4u){
        if (split_rain_seat >= 0) MR32(c->esp) = split_view_id(split_rain_seat);
        return 1;
    }
    if (ip == 0x6136c7u){
        if (split_racing() && c->eax >= 4 && c->eax <= 7) c->eax = (c->eax - 4) & 1;
        return 1;
    }
    if (ip == 0x614382u){
        if (split_racing() && c->ebp >= 4 && c->ebp <= 7) c->ebp = (c->ebp - 4) & 1;
        return 1;
    }
    if (ip == 0x61e73au){
        uint32_t view = MR32(c->esi + 0x1ec);
        if (split_racing() && view) for (uint32_t seat = 0; seat < (unsigned)split_render_limit(); ++seat)
            if (MR32(view + 4) == (uint32_t)split_view_id(seat)){
                c->eax = gcall(c, 0x5ed980u, G_RACEPARAMS, 1, &seat); break;
            }
        return 1;
    }
    if (ip == 0x5cb31fu){
        split_blur_seat = -1;
        if (split_racing()) for (int k = 0; k < split_render_limit(); ++k)
            if (split_current_view == split_view_id(k)) split_blur_seat = k;
        if (split_blur_seat >= 0){
            split_blur_saved = MR32(0x870900u);
            MRF32(0x870900u) = split_blur_strength[split_blur_seat];
            memset(split_blur_copy, 0, sizeof split_blur_copy);
        }
        return 1;
    }
    if (ip == 0x5cb324u){
        if (split_blur_seat >= 0){
            split_blur_strength[split_blur_seat] = MRF32(0x870900u);
            MR32(0x870900u) = split_blur_saved;
            split_blur_seat = -1;
            if (split_blur_copy[0]){
                uint32_t a[] = {split_blur_copy[1], 0, split_blur_copy[2], 0, split_blur_copy[3]};
                reflection_com(c, split_blur_copy[0], 34, 5, a);
            }
        }
        return 1;
    }
    if (ip == 0x5cc474u || ip == 0x5cc490u){
        if (split_blur_seat >= 0) MR32(c->esp) = split_current_view;
        return 1;
    }
    if (ip == 0x5cae12u){
        /* Outside split screen the game's own call here is the only one. */
        if (!split_racing()) return 1;
        gcall(c, 0x5cf420u, 0, 0, 0);
        int initialized = 0;
        if (split_racing()) for (int k = 1; k < split_render_limit(); ++k) if (!env_valid[k]){
            env_turn = k; reflection_env(c); initialized = 1;
        }
        /* Never display another player's cube while a new seat waits for
         * its first update; later refreshes are staggered like the ASI. */
        if (!initialized) reflection_env(c);
        return 1;
    }
    if (ip == 0x6312b0u && split_racing() && MR32(c->edi+4) == 4){
        unsigned seat = (unsigned)reflection_seat;
        c->esi = MR32(SPLIT_PLAYERS_INDEX + seat*4);
        c->eax = gcall(c, 0x5ed980u, G_RACEPARAMS, 1, &seat);
        return 1;
    }
    /* The native streak pass hard-codes view 1 and shares a persistent
     * full-screen texture. Give each camera its own region, including the
     * UVs used when compositing that texture back into a player viewport. */
    if (ip == 0x5cbc02u){
        if (split_racing() && split_current_view){
            MR32(c->esp) = split_current_view;
            static unsigned logged;
            if (getenv("NFSU2_SPLIT_LIGHTS_LOG") && !(logged & (1u << split_current_view))){
                logged |= 1u << split_current_view;
                fprintf(stderr, "[split lights] streak camera=%d, isolated accumulation, overbright=%u\n",
                        split_current_view, MR32(0x870cfcu));
            }
        }
        return 1;
    }
    if (ip == 0x5cbbb0u){ split_streak_full_copy = 1; return 1; }
    if (ip == 0x5cbbb5u){ split_streak_full_copy = 0; return 1; }
    if (ip == 0x5cbda6u){ split_streak_composite = split_racing() ? split_current_view : 0; return 1; }
    if (ip == 0x5cbdabu){ split_streak_composite = 0; return 1; }
    if (ip == 0x5c6ec6u){
        for (int k = 0; k < split_render_limit(); ++k) if (split_streak_composite == split_view_id(k)){
            unsigned w, h; void com_backbuffer_size(unsigned *, unsigned *);
            com_backbuffer_size(&w, &h);
            SplitCell cell = split_cell(k);
            /* At the call instruction, the three arguments precede the
             * caller's local quad: native hook's esp+0x40 minus return. */
            uint32_t vertices = c->esp + 0x3cu;
            for (unsigned v = 0; v < 4; ++v) for (unsigned t = 0; t < 4; ++t){
                uint32_t uv = vertices + v * 48u + 16u + t * 8u;
                MRF32(uv) = (cell.x + MRF32(uv) * cell.w) / w;
                MRF32(uv + 4) = (cell.y + MRF32(uv + 4) * cell.h) / h;
            }
        }
        return 1;
    }
    if ((ip == 0x606fe8u || ip == 0x5f6771u) && split_requested()){
        /* Native patch changes "exactly two" to "at least two": the
         * first finisher starts Hurry, and all humans receive catch-up. */
        c->zf = MR32(SPLIT_PLAYERS_COUNT) >= 2;
        return 1;
    }
    if (ip == 0x5f678cu && split_requested()){
        uint32_t self = MR32(c->esi + 0x18), other = 0;
        float best = -1e30f;
        for (unsigned k = 0; k < MR32(SPLIT_PLAYERS_COUNT) && k < 6; ++k){
            uint32_t p = MR32(SPLIT_PLAYERS_INDEX + k * 4), car = p ? MR32(p + 4) : 0;
            uint32_t racer = car ? MR32(car + 0x1c) : 0;
            if (p && p != self && racer && (!other || MRF32(racer + 0x14) > best)){
                other = p; best = MRF32(racer + 0x14);
            }
        }
        if (other) c->edx = other;
        return 1;
    }
    if (ip == 0x4174c5u && split_requested() > 2 && !MR32(c->ebx + 0x748)){
        uint32_t ai_racer = MR32(c->esp + 0x14), leader = 0, trailer = 0;
        int threshold = (int)MR32(0x89e7c4u) / 2, best = 127, worst = -128;
        for (unsigned k = 0; k < (unsigned)split_requested(); ++k){
            uint32_t p = MR32(SPLIT_PLAYERS_INDEX + k * 4), car = p ? MR32(p + 4) : 0;
            uint32_t racer = car ? MR32(car + 0x1c) : 0;
            if (!racer) continue;
            int pos = (int8_t)MR8(racer + 0xa);
            if (pos <= threshold) ++threshold;
            if (pos < best){ best = pos; leader = p; }
            if (pos > worst){ worst = pos; trailer = p; }
        }
        if (ai_racer) MR32(c->ebx + 0x748) = (int8_t)MR8(ai_racer + 0xa) > threshold ? trailer : leader;
        return 1;
    }
    if (ip == 0x51b0b0u && split_coop_hud() && MR32(c->esp + 16) == 0){
        /* co-op: a full-screen HUD (640x480, centred units) into its half: s times as big, an element left or right
         * of the middle kept on that screen edge, one above the middle at the half's top, one below at its bottom
         * (the names over online players' cars are placed in their half already: roam_game.c) */
        uint32_t data = MR32(c->esp + 4);
        int roam_game_placed(uint32_t data);
        if (roam_game_placed(data)) return 1;
        for (unsigned i = 0; data && i < split_hud_data_count; ++i) if (split_hud_data[i].data == data){
            uint32_t copy = 0x00d08000u + (split_hud_copy++ & 15) * 0x50;
            memcpy(g_mem + copy, g_mem + data, 0x50);
            unsigned width, height; void com_backbuffer_size(unsigned *, unsigned *);
            com_backbuffer_size(&width, &height);
            float x = MRF32(copy + 28), y = MRF32(copy + 32);
            /* player 2's HUD without the top band: the bank, the SMS and Engage icons, the song, the outrun meter are
             * player 1's career's */
            if (split_hud_data[i].hide || (split_hud_data[i].seat && y < -60.f && fabsf(x) < 400.f)) x = y = 10000.f;
            else if (fabsf(x) < 400.f && fabsf(y) < 300.f){             /* not one the game parks off screen */
                float sc = split_coop_scale(), e = height ? 240.f * width / height : 320.f;
                float top = split_hud_data[i].seat ? 0.f : -240.f, bot = top + 240.f;
                x = x < -40.f ? -e + (x + 320.f) * sc : x > 40.f ? e - (320.f - x) * sc : x * sc;
                y = y < 0.f ? top + (y + 240.f) * sc : bot - (240.f - y) * sc;
                for (unsigned k = 4; k <= 6; ++k) MRF32(copy + k * 4) *= sc;
                MRF32(copy + 56) *= sc; MRF32(copy + 60) *= sc;
            }
            MRF32(copy + 28) = x; MRF32(copy + 32) = y;
            MR32(c->esp + 4) = copy;
            break;
        }
        return 1;
    }
    if (ip == 0x51b0b0u && split_racing() && split_render_limit() > 2 && MR32(c->esp + 16) == 0){
        uint32_t data = MR32(c->esp + 4);
        for (unsigned i = 0; data && i < split_hud_data_count; ++i) if (split_hud_data[i].data == data){
            uint32_t copy = 0x00d08000u + (split_hud_copy++ & 15) * 0x50;
            memcpy(g_mem + copy, g_mem + data, 0x50);
            unsigned width, height; void com_backbuffer_size(unsigned *, unsigned *);
            com_backbuffer_size(&width, &height);
            SplitCell cell = split_cell(split_hud_data[i].seat);
            float cell_w = 480.f * cell.w / height, cell_h = 480.f * cell.h / height;
            float scale = fminf(cell_w / 640.f, cell_h / 240.f);
            float cx = 480.f * (cell.x + cell.w * 0.5f - width * 0.5f) / height;
            float cy = 480.f * (cell.y + cell.h * 0.5f) / height - 240.f;
            MRF32(copy + 28) = cx + MRF32(copy + 28) * scale;
            MRF32(copy + 32) = cy + (MRF32(copy + 32) + 120.f) * scale;
            /* Spare minimap icons are parked offscreen in the original HUD.
             * Scaling a whole HUD must not pull those icons into a cell. */
            if (fabsf(MRF32(copy + 32) - cy) > 160.f)
                MRF32(copy + 32) += MRF32(copy + 32) > cy ? 2000.f : -2000.f;
            for (unsigned k = 4; k <= 6; ++k) MRF32(copy + k * 4) *= scale;
            MRF32(copy + 56) *= scale; MRF32(copy + 60) *= scale;
            MR32(c->esp + 4) = copy;
            /* Hide cloned inactive pools; split_minimap.inc draws the live
             * manager's snapshots and explicitly selects extra car markers. */
            if (split_hud_data[i].hide) MRF32(copy + 28) = MRF32(copy + 32) = 10000.f;
            break;
        }
        return 1;
    }
    if (ip == 0x5b9caau){
        if (split_racing()) split_return_count=0;
        split_sync_settings(0);
        split_map_huds(c);
        split_world = c->esi;
        split_cull_base = 0;
        if (split_racing() && split_render_limit() > 2){
            uint32_t view = split_view(5), target = MR32(view + 0x58);
            unsigned count = MR32(split_world + 0x6c0);
            if (MR8(view + 8) && target && MR32(target + 8) && count < 4){
                uint32_t args[] = {view, split_world + count * 0x90, MR32(0x8707a4u) ? 0x1000u : 0x6000u};
                MR32(split_world + 0x6c0) = count + 1;
                gcall(c, 0x4888a0u, 0, 3, args);
            }
        }
        return 1;
    }
    if (ip == 0x498070u && MR32(G_GAMEMODE) == 4){
        uint32_t player = MR32(0x85ad28u), msg = MR32(c->esp + 4);
        if (player < 2){
            static uint32_t held[6];
            unsigned seat = (unsigned)split_seat;
            if (msg == 0xc98356bau){
                split_seat_title(c);
                if (MR8(c->ecx + 0x4c) && split_kb_of(seat) < 0){
                    int side = split_kb_seat[SPLIT_KB_ENTER] < 0 && (kbd_key_down(0x1c) || kbd_key_down(0x9c)) ? SPLIT_KB_ENTER
                             : split_kb_seat[SPLIT_KB_SPACE] < 0 && kbd_key_down(0x39) ? SPLIT_KB_SPACE : -1;
                    if (side >= 0){
                        split_kb_seat[side] = (int)seat;
                        fprintf(stderr, "[split] keyboard (%s) assigned to player %u\n", side ? "Space" : "Enter", seat + 1);
                    }
                }
                PadState pad;
                split_read_local(seat, &pad);
                uint32_t start = pad.buttons & (1u << PAD_START);
                uint32_t stamp = split_join_counter(seat);
                if (MR8(c->ecx + 0x4c) && ((start && !held[seat]) || (pad.connected && stamp != split_join_stamp))){
                    MR32(c->esp + 4) = 0xb5af2461u;
                    MR32(c->esp + 12) = 1u << player;
                    split_joined[seat] = 1;
                    split_release_pad = 1;
                    split_join_stamp = stamp;
                    fprintf(stderr, "[split] player %u joined\n", seat+1);
                }
                held[seat] = start;
            } else if (msg == 0xb5af2461u || msg == 0x0c407210u || msg == 0x9803f6e2u){
                MR32(c->esp + 4) = 0;
            }
        }
        return 1;
    }
    if (ip == 0x4fc1f0u && MR32(G_GAMEMODE) == 4){
        split_carsel_active = 1;
        uint32_t msg = MR32(c->esp+4);
        if (msg == 0xc98356bau) split_seat_title(c);
        if (msg == 0x911ab364u && split_seat >= 2 && !(split_seat & 1) && split_popup(c)){
            uint32_t pkg = SPLIT_FE_TEXT+256;
            gcall(c, 0x5379a0u, 0, 1, &pkg);
            split_restore_seat(0);
            split_switch_seat(c, c->ecx, split_seat-1);
            MR32(c->esp+4) = 0;
        }
        uint32_t player = MR32(0x85ad28u);
        if (player < 2){
            uint32_t port = MR32(0x7f4454u + player * 4);
            MR32(c->esp + 12) = port == 0xffffffffu ? 0 : port;
        }
        return 1;
    }
    /* right after UI_Main's AddOption of Quick Race (0x4AEED5): Split Screen is the item after it (the carousel
     * opens on Quick Race: one right), Online, LAN, Customize, Profile Manager and Options one place further */
    if (ip == 0x4aeed8u){ split_add_menu(c, c->esi); return 1; }
    if (ip == 0x4fc06bu){
        if (split_entering){
            /* PC mode 4 restores this backup on exit, so populate it FIRST (and the career garage's slots 6-10,
             * which the mode's exit puts back too: garage.c 0x515D70) */
            memcpy(g_mem + 0x83a9d0u + 0x1677cu, g_mem + 0x83a9d0u + 0x3c0u, 0x26f6u * 4);
            garage_split_keep(c);
            MR32(0x83a9d0u + 0x20354u) = MR32(0x83a9d0u + 0xe8u);
            MR32(G_GAMEMODE) = 4;
            for (unsigned i = 0; i < 13; ++i) MR8(0x83aac4u + i * 0x14u) = 2;
            MR32(0x85ad28u) = 0; MR32(0x7f4454u) = 1; MR32(0x7f4458u) = 2;
            split_seat = 0; split_carsel_active = 0;
            split_kb_seat[0] = split_kb_seat[1] = -1;
            memset(split_picked, 0, sizeof split_picked);
            memset(split_joined, 0, sizeof split_joined);
        } else {
            split_menu_players = 0; split_carsel_active = 0;
            MR32(0x85ad28u) = 0;
            MR32(0x7f4454u) = MR32(0x7f4458u) = 0xffffffffu;
            for (unsigned i = 0; i < 13; ++i) MR8(0x83aac4u + i * 0x14u) = 1;
        }
        return 1;
    }
    if (ip == 0x5dacdbu){ c->zf = 0; return 1; } /* full PC track sections */
    if (ip == 0x5cb004u){
        int seat = (int)c->edx - 1;
        if (split_racing() && seat >= 0 && seat < split_render_limit()){
            /* The native glare pools only have two counters. Consume and
             * reset them for each camera instead of sharing the last view. */
            MR32(0x82dae8u) = MR32(0x82daecu) = 0;
            split_world_for(c, seat);
            reflection_use(c, seat);
            /* View 9 is one shared projector. Its camera and visible mesh
             * list must be rebuilt before each player's headlight pass. */
            uint32_t v9 = 0x832de0u + 9u * 0x70u;
            if (MR32(0x870cc4u) && MR32(v9 + 8) && MR32(v9 + 0x40)){
                uint32_t saved = MR32(0x89e808u);
                MR32(0x89e808u) = seat < 2 ? MR32(0x89e808u + 4u*seat) : (uint32_t)seat;
                uint32_t a[] = {v9, 0};
                gcall(c, 0x5c2ae0u, 0, 2, a);
                MR32(0x89e808u) = saved;
                reflection_update(c, v9);
                reflection_recull(c, v9);
            }
            c->edx = split_view_id(seat);
            uint32_t rt = SPLIT_TARGETS + seat * 28u;
            MR32(rt + 8) = MR32(0x86e770u); MR32(rt + 12) = MR32(0x86e774u);
        }
        split_current_view = c->edx;
        return 1;
    }
    if (ip == 0x5cb72du){
        split_current_view = 0;
        /* the game's road reflection back, if a player's own is still selected (only ours are replaced: after a
         * split race the game's texture may be another one) */
        uint32_t now = MR32(0x870894u);
        if (refl_game_tex && now != refl_game_tex)
            for (int k = 1; k < 6; ++k) if (refl_tex[k] && now == refl_tex[k]){ MR32(0x870894u) = refl_game_tex; break; }
        return 1;
    }
    if (ip == 0x5c80b0u && split_racing()){
        uint32_t view = c->ebp, plat = c->esi, camera = MR32(view + 0x40), target = MR32(view + 0x58);
        int seat = -1;
        for (int k = 0; k < split_render_limit(); ++k) if (MR32(view + 4) == (unsigned)split_view_id(k)) seat = k;
        if (MR32(view + 4) == 4){
            seat = reflection_seat;
            target = SPLIT_TARGETS + seat * 28u;
        }
        if (seat >= 0 && camera && target && MR32(target + 24) && MR16(camera + 0xc4)){
            unsigned width, height; void com_backbuffer_size(unsigned *, unsigned *);
            com_backbuffer_size(&width, &height);
            float screen = (float)width / height;
            float aspect = (float)MR32(target + 20) / MR32(target + 24);
            float half = MR16(camera + 0xc4) * (3.14159265f / 65536.f);
            float p00 = (1.f / tanf(half)) * (4.f / 3.f) / (aspect < screen ? aspect : screen);
            MRF32(plat + 0x40) = p00; MRF32(plat + 0x54) = -p00 * aspect;
        }
        return 1;
    }
#endif
    return 0;
}

/* The streak accumulation is a full-screen atlas, just like the backbuffer.
 * Reflection/speed-blur targets remain local. The AA history copy alone
 * must cover the entire atlas before drawing the current camera's lights. */
int split_backbuffer_region(uint32_t surface, uint32_t rect[4]){
    if (!split_racing() || !surface) return 0;
    if (surface != MR32(0x86e770u) &&
        (split_streak_full_copy || (surface != MR32(0x86ff70u) && surface != MR32(0x86f2e0u)))) return 0;
    for (int k = 0; k < split_render_limit(); ++k) if (split_current_view == split_view_id(k)){
        SplitCell cell = split_cell(k);
        rect[0] = cell.x; rect[1] = cell.y; rect[2] = cell.w; rect[3] = cell.h;
        return 1;
    }
    return 0;
}



