/* dinput_kbd.c -- the system keyboard as an IDirectInputDevice8 (device
 * kind 1, from EnumDevicesBySemantics or CreateDevice(GUID_SysKeyboard)).
 *
 * How SPEED2.EXE (v1.2 NA) uses it (static analysis):
 *   - One keyboard device [0x86e5c0], from EnumDevicesBySemantics (callback
 *     0x5ca660 -> 0x5c1080).  0x5b9280 copies the default action tables
 *     (0x7fcac0 race, 0x7fd7e0 front end, 0x7fd330, 0x7fd650) into four
 *     DIACTIONFORMATs (0x86f348 + n*0x148), BuildActionMap(INITIALIZE) maps
 *     them, SetActionMap selects one; 0x5cf320(n) switches maps
 *     (Unacquire, SetActionMap(n, DIDSAM_FORCESAVE)).
 *   - Every frame 0x5c1840 reads GetDeviceData and uses uAppData (the
 *     action id) and dwData; DIERR_NOTACQUIRED/INPUTLOST -> Acquire.
 *   - Rebinding a key on the Controls page: 0x5c2100 -> 0x5b9120 switches
 *     the device to c_dfDIKeyboard (Unacquire, SetDataFormat(0x7bfdb0),
 *     DIPROP_BUFFERSIZE 16, Acquire); 0x5c15f0 then reads GetDeviceData
 *     and takes the first pressed key: dwOfs == 1 (DIK_ESCAPE) cancels,
 *     anything else goes through GetObjectInfo(dwOfs, DIPH_BYOFFSET) and
 *     its dwType becomes the action's dwObjID (0x5b8550, which also unbinds
 *     the key from any other action).  0x5cf320 x4 then restores the maps
 *     with DIDSAM_FORCESAVE, which is how DirectInput keeps keyboard
 *     bindings across runs (its per-user "user maps"; the game's profile
 *     only stores pad bindings, 0x5b9420).
 *
 * So the device keeps its own data format (none, a SetDataFormat format, or
 * an action map), acquisition state, buffer size and read position in the
 * key event ring (runtime/keyboard.c).  dwOfs is an offset in the CURRENT
 * format: the key's DIK code for c_dfDIKeyboard, 4 * action index for an
 * action map, as DirectInput 8 does.  Before, events always carried the
 * action-map offset, so the capture saw Escape (front-end action 11) as
 * offset 44 = DIK_Z and Enter (action 4) as 16 = DIK_Q, and SetActionMap
 * mapped every action by its semantic, ignoring the chosen dwObjID.
 *
 * User maps (DIDSAM_FORCESAVE / BuildActionMap) are kept in the runtime's
 * registry file (win32_ext.c) as "DIMap.{guidActionMap}.<actions>.<index>" =
 * "<semantic> <uAppData> <dwObjID>" for every action whose key differs from
 * the default, so they persist the way DirectInput's do: per user, for all
 * of the game's profiles. */
#include "cpu.h"
#include "kbd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t com_scratch(uint32_t n);
uint32_t com_devinst(int keyboard);                         /* com.c */
const char *dik_name(unsigned dik);                         /* dikeys.c */
void hfr_note_input_read(int pad);                          /* framerate.c: input age */
int  win32_reg_get_str(const char *name, char *out, unsigned cap);   /* win32_ext.c */
void win32_reg_set_str(const char *name, const char *v);
void win32_reg_del(const char *name);
void win32_reg_flush(void);

#define DI_OK                 0x00000000u
#define DI_NOEFFECT           0x00000001u
#define DI_BUFFEROVERFLOW     0x00000001u
#define DIERR_OBJECTNOTFOUND  0x80070002u
#define DIERR_NOTACQUIRED     0x8007000Cu
#define DIERR_INVALIDPARAM    0x80070057u
#define DIERR_UNSUPPORTED     0x80004001u

#define DIDFT_PSHBUTTON 0x00000004u
#define DIDFT_BUTTON    0x0000000Cu
#define DIDFT_ANYINST   0x00FFFF00u
#define DIGDD_PEEK      0x00000001u
#define DIDSAM_FORCESAVE 0x00000002u
#define DIDBAM_PRESERVE   0x00000001u
#define DIDBAM_INITIALIZE 0x00000002u
#define DIDBAM_HWDEFAULTS 0x00000004u
#define DIA_APPMAPPED   0x00000002u
#define DIA_APPNOMAP    0x00000004u
#define DIAH_UNMAPPED   0x00000000u
#define DIAH_USERCONFIG 0x00000001u
#define DIAH_APPREQUESTED 0x00000002u
#define DIAH_DEFAULT    0x00000020u
#define DIKEYBOARD_BASE 0x81000400u
#define KEY_OBJID(dik)  (DIDFT_PSHBUTTON | ((uint32_t)(dik) << 8))

static const uint8_t k_tail[8] = {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static void put_guid(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]){
    MR32(p) = d1; MR16(p + 4) = d2; MR16(p + 6) = d3;
    for (int i = 0; i < 8; i++) MR8(p + 8 + (uint32_t)i) = d4[i];
}
/* GUID_SysKeyboard {6F1D2B61-D5A0-11CF-BFC7-444553540000} */
static void put_kbd_guid(uint32_t p){ put_guid(p, 0x6F1D2B61u, 0xD5A0, 0x11CF, k_tail); }
static int is_kbd_guid(uint32_t p){
    if (!mem_is_committed(p + 15)) return 0;
    if (MR32(p) != 0x6F1D2B61u || MR16(p + 4) != 0xD5A0 || MR16(p + 6) != 0x11CF) return 0;
    for (int i = 0; i < 8; i++) if (MR8(p + 8 + (uint32_t)i) != k_tail[i]) return 0;
    return 1;
}
static void clear_guid(uint32_t p){ for (uint32_t i = 0; i < 16; i++) MR8(p + i) = 0; }

/* ---- device state ------------------------------------------------------- */
enum { FMT_NONE, FMT_RAW, FMT_ACTIONS };
typedef struct {
    uint32_t self;
    int      fmt;
    uint32_t datasize;
    int16_t  ofs_of[256];        /* FMT_RAW: data offset of each key, -1 = not in the format */
    int16_t  act_of[256];        /* FMT_ACTIONS: action index of each key, -1 = unmapped */
    uint32_t app_of[256];        /* FMT_ACTIONS: uAppData */
    uint32_t def_of[256];        /* FMT_ACTIONS: the action the map's semantics give the key (its default), 0xFFFFFFFF none */
    uint32_t af;                 /* FMT_ACTIONS: the action format (the actions' names) */
    uint32_t nact;
    uint32_t bufsize;
    int      acquired;
    uint32_t cursor;             /* next key event (kbd_ev_*) to report */
    uint64_t events;
} KDev;
static KDev g_kd[8];
static int  g_nkd;
static uint64_t g_maps_saved, g_maps_loaded;

static KDev *kdev(uint32_t self){
    for (int i = 0; i < g_nkd; i++) if (g_kd[i].self == self) return &g_kd[i];
    int i = g_nkd < 8 ? g_nkd++ : 7;           /* COM objects are never reused */
    KDev *d = &g_kd[i];
    memset(d, 0, sizeof *d);
    d->self = self;
    for (int k = 0; k < 256; k++){ d->ofs_of[k] = -1; d->act_of[k] = -1; }
    return d;
}
void dikbd_created(uint32_t self){ kdev(self); }

static int lg(void){ return kbd_log() || getenv("DIMAP"); }

/* the keys a PC keyboard has: the ones with a name */
static int key_exists(unsigned dik){ return dik > 0 && dik < 256 && dik_name(dik) != 0; }

static const char *key_label(unsigned dik){
    const char *n = kbdmac_layout_name(dik);
    if (!n) n = dik_name(dik);
    return n;
}
static void write_objinst(uint32_t p, uint32_t size, unsigned dik){
    if (size < 0x18u || size > 0x13cu) size = 0x13cu;
    mem_commit(p, size);
    memset(g_mem + p, 0, size);
    MR32(p) = size;
    put_guid(p + 0x04, 0x55728220u, 0xD33C, 0x11CF, k_tail);  /* GUID_Key */
    MR32(p + 0x14) = dik;                    /* offset in the native format = scan code */
    MR32(p + 0x18) = KEY_OBJID(dik);
    MR32(p + 0x1c) = 0;
    if (size >= 0x124u){
        const char *n = key_label(dik);
        if (n) snprintf((char *)(g_mem + p + 0x20), 260, "%s", n);
        else snprintf((char *)(g_mem + p + 0x20), 260, "Key %u", dik);
    }
    if (size >= 0x134u){ MR16(p + 0x130) = 7; MR16(p + 0x132) = 0; }   /* HID Keyboard page */
}

/* SetDataFormat: DIDATAFORMAT +12 dwDataSize +16 dwNumObjs +20 rgodf;
 * DIOBJECTDATAFORMAT (16): +0 pguid +4 dwOfs +8 dwType +12 dwFlags. */
static uint32_t set_format(KDev *d, uint32_t df){
    if (!df || !mem_is_committed(df + 23)) return DIERR_INVALIDPARAM;
    uint32_t osz = MR32(df + 4), dsz = MR32(df + 12), nobj = MR32(df + 16), rg = MR32(df + 20);
    if (osz < 16u || dsz == 0 || dsz > 4096u || nobj > 1024u) return DIERR_INVALIDPARAM;
    for (int k = 0; k < 256; k++) d->ofs_of[k] = -1;
    unsigned next_any = 1;
    for (uint32_t i = 0; i < nobj; i++){
        uint32_t o = rg + i * osz;
        if (!mem_is_committed(o + 15)) break;
        uint32_t ofs = MR32(o + 4), type = MR32(o + 8);
        if (!(type & DIDFT_BUTTON) && (type & 0xFFu) != 0) continue;   /* keyboards have buttons only */
        unsigned inst = (type >> 8) & 0xFFFFu, dik = 0;
        if ((type & DIDFT_ANYINST) == DIDFT_ANYINST){
            while (next_any < 256 && (!key_exists(next_any) || d->ofs_of[next_any] >= 0)) next_any++;
            if (next_any >= 256) continue;
            dik = next_any++;
        } else dik = inst;
        if (dik == 0 || dik > 255 || ofs >= dsz) continue;
        d->ofs_of[dik] = (int16_t)ofs;
    }
    d->fmt = FMT_RAW; d->datasize = dsz;
    MR32(d->self + 4) = df;
    d->cursor = kbd_ev_head();
    if (lg()) fprintf(stderr, "[dikbd] SetDataFormat(0x%08x): %u objects, %u bytes\n", df, nobj, dsz);
    return DI_OK;
}

/* ---- action maps ----------------------------------------------------------
 * DIACTIONFORMATA: +4 dwActionSize +8 dwDataSize +12 dwNumActions
 * +16 rgoAction +20 guidActionMap +40 dwBufferSize.
 * DIACTIONA (40): +0 uAppData +4 dwSemantic +8 dwFlags +16 guidInstance
 * +32 dwObjID +36 dwHow. */
typedef struct { uint32_t nact, rg, asz; char guid[40]; } AF;
static int read_af(uint32_t af, AF *f){
    if (!af || !mem_is_committed(af + 71)) return 0;
    f->asz = MR32(af + 4); if (f->asz < 40u) f->asz = 40u;
    f->nact = MR32(af + 12); f->rg = MR32(af + 16);
    if (f->nact > 1024u || (f->nact && !f->rg)) return 0;
    snprintf(f->guid, sizeof f->guid, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             MR32(af + 20), MR16(af + 24), MR16(af + 26), MR8(af + 28), MR8(af + 29), MR8(af + 30),
             MR8(af + 31), MR8(af + 32), MR8(af + 33), MR8(af + 34), MR8(af + 35));
    return 1;
}
static unsigned sem_key(uint32_t sem){
    return (sem & 0xFFFFFF00u) == DIKEYBOARD_BASE ? (sem & 0xFFu) : 0u;
}
/* the default key of every action: its keyboard semantic, each key given to
 * the first action that asks for it (DirectInput maps an object once) */
static void default_map(const AF *f, uint32_t *objid){
    uint8_t used[256] = {0};
    for (uint32_t k = 0; k < f->nact; k++){
        uint32_t a = f->rg + k * f->asz;
        objid[k] = 0;
        if (!mem_is_committed(a + 39)) continue;
        unsigned dik = sem_key(MR32(a + 4));
        if (dik && key_exists(dik) && !used[dik]){ used[dik] = 1; objid[k] = KEY_OBJID(dik); }
    }
}
static void reg_name(char *nm, unsigned cap, const AF *f, uint32_t k){
    snprintf(nm, cap, "DIMap.%s.%u.%u", f->guid, f->nact, k);
}
static void save_user_map(const AF *f){
    uint32_t def[1024];
    default_map(f, def);
    int changed = 0;
    for (uint32_t k = 0; k < f->nact; k++){
        uint32_t a = f->rg + k * f->asz;
        if (!mem_is_committed(a + 39)) break;
        uint32_t cur = (MR32(a + 36) != DIAH_UNMAPPED && is_kbd_guid(a + 16)) ? MR32(a + 32) : 0u;
        char nm[96], val[64], old[128];
        reg_name(nm, sizeof nm, f, k);
        int have = win32_reg_get_str(nm, old, sizeof old);
        /* a key this keyboard does not have (the game's reset gives two
         * debug actions DIK 0xEA/0xE5) is no binding worth keeping */
        if (cur && !key_exists((cur >> 8) & 0xFFu) && !def[k]) cur = 0;
        if (cur == def[k]){
            if (have){ win32_reg_del(nm); changed = 1; }
            continue;
        }
        snprintf(val, sizeof val, "%08x %08x %08x", MR32(a + 4), MR32(a), cur);
        if (!have || strcmp(old, val)){ win32_reg_set_str(nm, val); changed = 1;
            if (lg()) fprintf(stderr, "[dikbd] user map %s = %s\n", nm, val); }
    }
    if (changed){ win32_reg_flush(); g_maps_saved++; }
}
/* the saved key of action k, or -1 */
static int64_t load_user(const AF *f, uint32_t k, uint32_t a){
    char nm[96], val[128];
    reg_name(nm, sizeof nm, f, k);
    if (!win32_reg_get_str(nm, val, sizeof val)) return -1;
    unsigned sem = 0, app = 0, obj = 0;
    if (sscanf(val, "%x %x %x", &sem, &app, &obj) != 3) return -1;
    if (sem != MR32(a + 4) || app != MR32(a)) return -1;           /* another game version's map */
    if (obj && !key_exists((obj >> 8) & 0xFFu)) return -1;
    return obj;
}
static uint32_t build_map(KDev *d, uint32_t af, uint32_t flags){
    AF f;
    if (!read_af(af, &f)){
        if (lg()) fprintf(stderr, "[dikbd] BuildActionMap(0x%08x): not a valid action format\n", af);
        return DIERR_INVALIDPARAM; }
    if (!f.nact){
        if (lg()) fprintf(stderr, "[dikbd] BuildActionMap(0x%08x): no actions\n", af);
        return DI_NOEFFECT; }
    uint32_t def[1024];
    default_map(&f, def);
    /* DIDBAM_INITIALIZE overrides what the application asked for
     * (DIA_APPMAPPED, DIA_APPNOMAP), as DirectX documents it.  The game
     * relies on it: it marks its first race action, Throttle on the Up
     * arrow, DIA_APPNOMAP (0x5c1100) and builds with INITIALIZE, and Up is
     * a throttle key on its Controls page. */
    int user = !(flags & DIDBAM_HWDEFAULTS), app_rules = !(flags & DIDBAM_INITIALIZE), nuser = 0;
    for (uint32_t k = 0; k < f.nact; k++){
        uint32_t a = f.rg + k * f.asz;
        if (!mem_is_committed(a + 39)) break;
        uint32_t fl = app_rules ? MR32(a + 8) : 0u;
        if (!sem_key(MR32(a + 4)) && !(fl & DIA_APPMAPPED)) continue;   /* not a keyboard action */
        if ((flags & DIDBAM_PRESERVE) && MR32(a + 36) != DIAH_UNMAPPED) continue;
        if ((fl & DIA_APPMAPPED) && is_kbd_guid(a + 16)){ MR32(a + 36) = DIAH_APPREQUESTED; continue; }
        if (fl & DIA_APPNOMAP){ clear_guid(a + 16); MR32(a + 32) = 0; MR32(a + 36) = DIAH_UNMAPPED; continue; }
        int64_t u = user ? load_user(&f, k, a) : -1;
        uint32_t obj = u >= 0 ? (uint32_t)u : def[k];
        if (u >= 0) nuser++;
        if (obj){ put_kbd_guid(a + 16); MR32(a + 32) = obj;
                  MR32(a + 36) = u >= 0 ? DIAH_USERCONFIG : DIAH_DEFAULT; }
        else    { clear_guid(a + 16); MR32(a + 32) = 0; MR32(a + 36) = DIAH_UNMAPPED; }
    }
    if (nuser) g_maps_loaded++;
    if (lg() || nuser)
        fprintf(stderr, "[dikbd] BuildActionMap %s: %u actions, %d from the saved user map\n",
                f.guid, f.nact, nuser);
    (void)d;
    return DI_OK;
}
/* ---- our keys ----------------------------------------------------------------
 * The keys the runtime and the page add on top of the game's: X drift mode (drift.c), O the show-off (showoff.c), N
 * the horn (horn.c), G the headlights (lightkey.c), and the page's I (Message a friend: web/src/app/messages.ts), F
 * (Add friend: friends.ts), 1 to 4 and Numpad 1 to 4 (quick chat: quickchat.ts).  None is one of the game's in its
 * driving map (0x7fcac0; G and Numpad 1 to 4 are its debug camera's, which the game never reads), but a player can
 * put any of the game's controls on any key (Options > Controls > Keyboard Configuration).  Then ours leave that key
 * to the game, for as long as the action map the keyboard reads has it so: the key's action there is not its default
 * (the action the map's semantics give it).  Each says so once in the log ("[keys] X is bound to Shift Up in the
 * game: drift mode stays off it").  The page's controls hear it through the status "keys" (status_web.c's block): a
 * bit for each of PAGE_KEYS, in its order, set while the game has that key.
 * The player may give ours other keys (the start page's Controls, Site: settings.ini key_drift ..., NFSU2_KEY_DRIFT
 * ...: dikbd_site_key).  The page offers none the game has a control on by default; one set by hand in the file is
 * left to the game the same way when its default is one of the game's live controls (not the debug camera's). */
#define NO_ACT 0xFFFFFFFFu
#define ACT_DEBUG(a) ((a) >= 0x31u && (a) <= 0x3eu)   /* the debug camera's (0x7fcac0: Numpad 1-9, L, [, ], G ...) */
__attribute__((weak)) int host_status_slot(const char *name){ (void)name; return -1; }
__attribute__((weak)) void host_status_set_n(const int *slot, const double *value, int n){ (void)slot; (void)value; (void)n; }
static const KDev *g_mapped;     /* the device last given an action map: what the keyboard reads now */
static uint32_t rebound(const KDev *d, unsigned dik){
    if (dik >= 256u || d->act_of[dik] < 0) return NO_ACT;
    uint32_t app = d->app_of[dik];
    return app != d->def_of[dik] || !ACT_DEBUG(app) ? app : NO_ACT;
}

/* KeyboardEvent.code (the page's names: web/src/input/keymap.ts) -> DIK, for the keys our controls may have */
static const struct { const char *code; uint8_t dik; } k_codes[] = {
    {"Digit1",0x02},{"Digit2",0x03},{"Digit3",0x04},{"Digit4",0x05},{"Digit5",0x06},{"Digit6",0x07},{"Digit7",0x08},
    {"Digit8",0x09},{"Digit9",0x0A},{"Digit0",0x0B},{"Minus",0x0C},{"Equal",0x0D},
    {"KeyQ",0x10},{"KeyW",0x11},{"KeyE",0x12},{"KeyR",0x13},{"KeyT",0x14},{"KeyY",0x15},{"KeyU",0x16},{"KeyI",0x17},
    {"KeyO",0x18},{"KeyP",0x19},{"BracketLeft",0x1A},{"BracketRight",0x1B},{"KeyA",0x1E},{"KeyS",0x1F},{"KeyD",0x20},
    {"KeyF",0x21},{"KeyG",0x22},{"KeyH",0x23},{"KeyJ",0x24},{"KeyK",0x25},{"KeyL",0x26},{"Semicolon",0x27},{"Quote",0x28},
    {"Backquote",0x29},{"Backslash",0x2B},{"KeyZ",0x2C},{"KeyX",0x2D},{"KeyC",0x2E},{"KeyV",0x2F},{"KeyB",0x30},
    {"KeyN",0x31},{"KeyM",0x32},{"Comma",0x33},{"Period",0x34},{"Slash",0x35},{"NumpadMultiply",0x37},
    {"F1",0x3B},{"F2",0x3C},{"F3",0x3D},{"F4",0x3E},{"F5",0x3F},{"F6",0x40},{"F7",0x41},{"F8",0x42},{"F9",0x43},{"F10",0x44},
    {"ScrollLock",0x46},{"Numpad7",0x47},{"Numpad8",0x48},{"Numpad9",0x49},{"NumpadSubtract",0x4A},{"Numpad4",0x4B},
    {"Numpad5",0x4C},{"Numpad6",0x4D},{"NumpadAdd",0x4E},{"Numpad1",0x4F},{"Numpad2",0x50},{"Numpad3",0x51},
    {"Numpad0",0x52},{"NumpadDecimal",0x53},{"IntlBackslash",0x56},{"F11",0x57},{"F12",0x58},{"F13",0x64},{"F14",0x65},
    {"F15",0x66},{"NumpadDivide",0xB5},{"Pause",0xC5},{"Home",0xC7},{"PageUp",0xC9},{"End",0xCF},{"PageDown",0xD1},
    {"Insert",0xD2},{"Delete",0xD3},
};
/* drift.c, showoff.c, horn.c, lightkey.c, page_keys: the key the variable env (NFSU2_KEY_DRIFT ...) gives one of ours:
 * its DIK, 0 for off; unset, or a name it does not know (said once), dflt */
unsigned dikbd_site_key(const char *env, unsigned dflt){
    const char *v = getenv(env);
    if (!v || !*v) return dflt;
    if (!strcasecmp(v, "off")) return 0;
    for (unsigned i = 0; i < sizeof k_codes / sizeof k_codes[0]; i++) if (!strcmp(v, k_codes[i].code)) return k_codes[i].dik;
    fprintf(stderr, "[keys] %s=%s: no such key, %s\n", env, v, dik_name(dflt) ? dik_name(dflt) : "the default");
    return dflt;
}
/* an action's name in the device's action format ("Shift Up") */
static const char *act_name(const KDev *d, uint32_t app){
    static char nm[48];
    AF f;
    snprintf(nm, sizeof nm, "action 0x%02x", app);
    if (!read_af(d->af, &f)) return nm;
    for (uint32_t k = 0; k < f.nact; k++){
        uint32_t a = f.rg + k * f.asz, s = 0;
        if (!mem_is_committed(a + 39)) break;
        if (MR32(a) != app || !(s = MR32(a + 12)) || !mem_is_committed(s)) continue;
        int i = 0;
        for (; i + 1 < (int)sizeof nm && mem_is_committed(s + (uint32_t)i) && MR8(s + (uint32_t)i) >= 0x20; i++) nm[i] = (char)MR8(s + (uint32_t)i);
        if (i) nm[i] = 0;
        break;
    }
    return nm;
}
static void said_once(unsigned dik, uint32_t app, const char *name, const char *what){
    static struct { uint16_t dik; uint32_t app; } said[32];
    static int n;
    for (int i = 0; i < n; i++) if (said[i].dik == dik && said[i].app == app) return;
    if (n < 32){ said[n].dik = (uint16_t)dik; said[n].app = app; n++; }
    fprintf(stderr, "[keys] %s is bound to %s in the game: %s stays off it\n", dik_name(dik) ? dik_name(dik) : "a key", name, what);
}
/* drift.c, showoff.c, horn.c: whether our control `what` takes the key dik now (1), or leaves it to the game (0) */
int dikbd_ours(unsigned dik, const char *what){
    const KDev *d = g_mapped;
    uint32_t app = d && d->fmt == FMT_ACTIONS ? rebound(d, dik) : NO_ACT;
    if (app == NO_ACT) return 1;
    said_once(dik, app, act_name(d, app), what);
    return 0;
}
/* the page's controls with their keys: the status "keys" has a bit for each, in this order (web/src/app/gamekeys.ts);
 * the first two the player may move (env, 0: off) */
static struct { uint8_t dik; const char *what, *env; } PAGE_KEYS[] = {
    { 0x17, "Message a friend", "NFSU2_KEY_MESSAGE" }, { 0x21, "Add friend", "NFSU2_KEY_FRIEND" },
    { 0x02, "quick chat", 0 }, { 0x03, "quick chat", 0 }, { 0x04, "quick chat", 0 }, { 0x05, "quick chat", 0 },     /* 1-4 */
    { 0x4F, "quick chat", 0 }, { 0x50, "quick chat", 0 }, { 0x51, "quick chat", 0 }, { 0x4B, "quick chat", 0 },     /* Numpad */
};
static void page_keys(const KDev *d){
    static int slot = -2, set;
    static double last = -1;
    uint32_t bits = 0;
    g_mapped = d;
    if (!set){ set = 1;
        for (unsigned i = 0; i < sizeof PAGE_KEYS / sizeof PAGE_KEYS[0]; i++)
            if (PAGE_KEYS[i].env) PAGE_KEYS[i].dik = (uint8_t)dikbd_site_key(PAGE_KEYS[i].env, PAGE_KEYS[i].dik); }
    for (unsigned i = 0; i < sizeof PAGE_KEYS / sizeof PAGE_KEYS[0]; i++){
        if (!PAGE_KEYS[i].dik) continue;
        uint32_t app = rebound(d, PAGE_KEYS[i].dik);
        if (app == NO_ACT) continue;
        bits |= 1u << i;
        said_once(PAGE_KEYS[i].dik, app, act_name(d, app), PAGE_KEYS[i].what);
    }
    if (slot == -2) slot = host_status_slot("keys");
    if (slot >= 0 && (double)bits != last){ double v = bits; last = v; host_status_set_n(&slot, &v, 1); }
}

static uint32_t set_map(KDev *d, uint32_t af, uint32_t flags){
    AF f;
    if (!read_af(af, &f)) return DIERR_INVALIDPARAM;
    for (int k = 0; k < 256; k++){ d->act_of[k] = -1; d->app_of[k] = 0; d->def_of[k] = 0xFFFFFFFFu; }
    d->af = af;
    int n = 0;
    for (uint32_t k = 0; k < f.nact; k++){
        uint32_t a = f.rg + k * f.asz;
        if (!mem_is_committed(a + 39)) break;
        unsigned sk = sem_key(MR32(a + 4));                       /* (default_map: the first action asking for a key) */
        if (sk && d->def_of[sk] == 0xFFFFFFFFu) d->def_of[sk] = MR32(a);
        if (MR32(a + 36) == DIAH_UNMAPPED || !is_kbd_guid(a + 16)){
            if (lg() && sem_key(MR32(a + 4)))
                fprintf(stderr, "[dikbd]   action %u (app 0x%x, key 0x%02x) unmapped: dwHow 0x%x dwObjID 0x%x guid %08x\n",
                        k, MR32(a), sem_key(MR32(a + 4)), MR32(a + 36), MR32(a + 32), MR32(a + 16));
            continue;
        }
        uint32_t obj = MR32(a + 32);
        unsigned dik = (obj >> 8) & 0xFFFFu;
        if (!(obj & DIDFT_BUTTON) || dik == 0 || dik > 255 || d->act_of[dik] >= 0) continue;
        d->act_of[dik] = (int16_t)k; d->app_of[dik] = MR32(a);
        n++;
        if (getenv("DIMAP"))
            fprintf(stderr, "  [map] dik=0x%02x -> action 0x%x (ofs %u)\n", dik, MR32(a), k * 4u);
    }
    d->fmt = FMT_ACTIONS; d->nact = f.nact;
    d->datasize = MR32(af + 8) ? MR32(af + 8) : 4u * f.nact;
    if (MR32(af + 40)) d->bufsize = MR32(af + 40);
    d->cursor = kbd_ev_head();
    page_keys(d);
    if (flags & DIDSAM_FORCESAVE) save_user_map(&f);
    if (lg()) fprintf(stderr, "[dikbd] SetActionMap %s: %u actions, %d keys mapped%s\n",
                      f.guid, f.nact, n, (flags & DIDSAM_FORCESAVE) ? ", saved" : "");
    return DI_OK;
}

/* ---- objects ---------------------------------------------------------------- */
/* DIPH_BYOFFSET: an offset in the current data format */
static int key_by_offset(const KDev *d, uint32_t ofs){
    if (d->fmt == FMT_RAW){
        for (int k = 1; k < 256; k++) if (d->ofs_of[k] >= 0 && (uint32_t)d->ofs_of[k] == ofs) return k;
        return -1;
    }
    if (d->fmt == FMT_ACTIONS){
        if (ofs & 3u) return -1;
        for (int k = 1; k < 256; k++) if (d->act_of[k] >= 0 && (uint32_t)d->act_of[k] * 4u == ofs) return k;
        return -1;
    }
    return ofs < 256u && key_exists(ofs) ? (int)ofs : -1;
}
static int key_by_how(const KDev *d, uint32_t obj, uint32_t how){
    if (how == 1u) return key_by_offset(d, obj);                  /* DIPH_BYOFFSET */
    if (how == 2u){                                               /* DIPH_BYID */
        unsigned dik = (obj >> 8) & 0xFFFFu;
        return (obj & DIDFT_BUTTON) && dik < 256 && key_exists(dik) ? (int)dik : -1;
    }
    return -1;
}

/* ---- methods ------------------------------------------------------------------- */
uint32_t dikbd_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *a){
    KDev *d = kdev(self);
    (void)nargs;
    if (!strcmp(nm, "GetDeviceData")){
        uint32_t cb = a[1], rg = a[2], pn = a[3], fl = a[4];
        hfr_note_input_read(0);
        if (!d->acquired) return DIERR_NOTACQUIRED;
        if (!pn || !mem_is_committed(pn + 3) || (rg && (cb < 16u || cb > 64u))) return DIERR_INVALIDPARAM;
        if (d->fmt == FMT_NONE){ MR32(pn) = 0; return DIERR_INVALIDPARAM; }
        uint32_t want = MR32(pn), n = 0, s = d->cursor, head = kbd_ev_head(), r = DI_OK;
        while (s != head && n < want){
            KbdEv e; int g = kbd_ev_get(s, &e);
            if (g < 0){ r = DI_BUFFEROVERFLOW; s++; continue; }
            if (g == 0) break;
            s++;
            uint32_t ofs, app = 0xFFFFFFFFu;
            if (d->fmt == FMT_RAW){
                if (d->ofs_of[e.dik] < 0) continue;
                ofs = (uint32_t)d->ofs_of[e.dik];
            } else {
                if (d->act_of[e.dik] < 0) continue;
                ofs = 4u * (uint32_t)d->act_of[e.dik]; app = d->app_of[e.dik];
            }
            if (rg){
                uint32_t p = rg + n * cb;
                mem_commit(p, cb); memset(g_mem + p, 0, cb);
                MR32(p) = ofs; MR32(p + 4) = e.down ? 0x80u : 0u;
                MR32(p + 8) = e.ms; MR32(p + 12) = e.seq;
                if (cb >= 20u) MR32(p + 16) = app;
            }
            n++;
            if (!(fl & DIGDD_PEEK)) d->events++;
            if (kbd_log() && !(fl & DIGDD_PEEK))
                fprintf(stderr, "[dikbd] event %s key 0x%02x -> dwOfs %u%s app 0x%x\n",
                        e.down ? "down" : "up  ", e.dik, ofs, d->fmt == FMT_RAW ? " (raw)" : "", app);
        }
        if (!(fl & DIGDD_PEEK)) d->cursor = s;
        MR32(pn) = n;
        return r;
    }
    if (!strcmp(nm, "GetDeviceState")){
        uint32_t cb = a[1], p = a[2];
        hfr_note_input_read(0);
        if (!d->acquired) return DIERR_NOTACQUIRED;
        if (!p || !cb || cb > 4096u) return DIERR_INVALIDPARAM;
        uint8_t st[256]; kbd_state(st, 256);
        mem_commit(p, cb); memset(g_mem + p, 0, cb);
        for (int k = 1; k < 256; k++){
            if (!st[k]) continue;
            if (d->fmt == FMT_RAW && d->ofs_of[k] >= 0 && (uint32_t)d->ofs_of[k] < cb) MR8(p + (uint32_t)d->ofs_of[k]) = 0x80;
            else if (d->fmt == FMT_ACTIONS && d->act_of[k] >= 0 && 4u * (uint32_t)d->act_of[k] + 3u < cb)
                MR32(p + 4u * (uint32_t)d->act_of[k]) = 0x80;
            else if (d->fmt == FMT_NONE && cb == 256u) MR8(p + (uint32_t)k) = 0x80;
        }
        return DI_OK;
    }
    if (!strcmp(nm, "Acquire")){
        if (d->acquired) return DI_NOEFFECT;
        d->acquired = 1; d->cursor = kbd_ev_head();      /* nothing is buffered while unacquired */
        return DI_OK;
    }
    if (!strcmp(nm, "Unacquire")){
        if (!d->acquired) return DI_NOEFFECT;
        d->acquired = 0;
        return DI_OK;
    }
    if (!strcmp(nm, "Poll")) return DI_NOEFFECT;           /* not a polled device */
    if (!strcmp(nm, "SetDataFormat")) return set_format(d, a[1]);
    if (!strcmp(nm, "BuildActionMap")) return build_map(d, a[1], a[3]);
    if (!strcmp(nm, "SetActionMap")) return set_map(d, a[1], a[3]);
    if (!strcmp(nm, "SetCooperativeLevel") || !strcmp(nm, "SetEventNotification")) return DI_OK;
    if (!strcmp(nm, "EnumObjects")){
        uint32_t cb = a[1], ref = a[2], fl = a[3];
        if (!cb) return DIERR_INVALIDPARAM;
        if (fl && !(fl & DIDFT_BUTTON)) return DI_OK;          /* axes, POVs...: none */
        static uint32_t oi = 0;
        if (!oi) oi = com_scratch(0x140);
        for (unsigned k = 1; k < 256; k++){
            if (!key_exists(k)) continue;
            write_objinst(oi, 0x13cu, k);
            uint32_t args[2] = { oi, ref };
            if (call_guest(c, cb, 2, args) == 0u) break;       /* DIENUM_STOP */
        }
        return DI_OK;
    }
    if (!strcmp(nm, "GetObjectInfo")){
        uint32_t p = a[1], obj = a[2], how = a[3];
        if (!p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        int k = key_by_how(d, obj, how);
        if (k < 0) return DIERR_OBJECTNOTFOUND;
        write_objinst(p, MR32(p), (unsigned)k);
        return DI_OK;
    }
    if (!strcmp(nm, "GetDeviceInfo")){
        uint32_t p = a[1];
        if (!p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        uint32_t sz = MR32(p); if (!sz || sz > 580u) sz = 580u;
        uint32_t src = com_devinst(1);
        mem_commit(p, sz); memcpy(g_mem + p, g_mem + src, sz); MR32(p) = sz;
        return DI_OK;
    }
    if (!strcmp(nm, "GetCapabilities")){
        uint32_t p = a[1];
        if (!p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        uint32_t sz = MR32(p); if (sz < 0x18u || sz > 0x2cu) sz = 0x2cu;
        mem_commit(p, sz); memset(g_mem + p, 0, sz); MR32(p) = sz;
        MR32(p + 0x04) = 0x00000001u;                         /* DIDC_ATTACHED */
        MR32(p + 0x08) = 0x0413u;                             /* KEYBOARD, PCENH */
        unsigned n = 0; for (unsigned k = 1; k < 256; k++) n += key_exists(k);
        MR32(p + 0x10) = n;                                   /* dwButtons */
        return DI_OK;
    }
    if (!strcmp(nm, "SetProperty") || !strcmp(nm, "GetProperty")){
        int set = nm[0] == 'S';
        uint32_t prop = a[1], h = a[2];
        if (!h || !mem_is_committed(h + 0x13)) return DIERR_INVALIDPARAM;
        if (prop == 1u){                                      /* DIPROP_BUFFERSIZE */
            if (set) d->bufsize = MR32(h + 16); else MR32(h + 16) = d->bufsize;
            return DI_OK;
        }
        if (!set && prop == 3u){ MR32(h + 16) = 1; return DI_OK; }   /* GRANULARITY */
        return set ? DI_OK : DIERR_UNSUPPORTED;
    }
    if (!strcmp(nm, "GetImageInfo") || !strcmp(nm, "RunControlPanel") || !strcmp(nm, "CreateEffect"))
        return DIERR_UNSUPPORTED;
    return DI_OK;
}

void dikbd_report(void){
    for (int i = 0; i < g_nkd; i++){
        KDev *d = &g_kd[i];
        fprintf(stderr, "  DI keyboard 0x%08x: %s, %llu events delivered, buffer %u, %s\n", d->self,
                d->fmt == FMT_RAW ? "SetDataFormat" : d->fmt == FMT_ACTIONS ? "action map" : "no format",
                (unsigned long long)d->events, d->bufsize, d->acquired ? "acquired" : "not acquired");
    }
    if (g_maps_saved || g_maps_loaded)
        fprintf(stderr, "  DI user maps       : %llu saved, %llu loaded\n",
                (unsigned long long)g_maps_saved, (unsigned long long)g_maps_loaded);
}
