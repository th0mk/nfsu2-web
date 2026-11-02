/* online.c -- the game's Online menu logs in under the player's name.
 *
 * The browser's server is its own stand-in for EA's servers (web/server-rs/crates/online,
 * runtime/winsock.c): any name logs in, there are no accounts.  So the EA Login
 * screens (Create New / Use Existing EA Account, the account name and password,
 * "save the login in a profile?", Select An Online Persona) ask for nothing the
 * server needs.  With online on, main menu -> Online goes straight to the
 * Online Main Menu as the player's name: roam_name (NFSU2_ROAM_NAME, the name
 * friends see in free roam), else the career profile's name.  That screen logs
 * in by itself, as it does for LAN play.  Without a usable name, or with online
 * off (and always on the Mac), the game's own screens come as before.
 *
 * The front end (SPEED2.EXE v1.2 NA, static reading):
 *  - The main menu's Online (0x4f98b0, message 0xd05fc3a3) sets the online mode
 *    [0x850078] = 8 and calls 0x4effd0(fe), which builds the login screens'
 *    flow ([0x836370], 0x4e3960: UI_OLISPConnect -> UI_OLEALogin ->
 *    UI_OLCreateUser | UI_OLUseExisting -> UI_OLSelectPersona -> UI_OLMAIN)
 *    and shows its first screen.
 *  - The login itself is a stage machine on the profile object 0x850080:
 *    stage +0x50, the stage before +0x54, an error flag +0x4c; 0x5031b0(n)
 *    sets the stage and clears the flag.  0x4cfee0(target): 2/3 -> 4, 4
 *    connect (LobbyConnect) -> 5 -> 6, auth (account +0x58, password +0x80,
 *    16 bytes each in online mode) -> 7 -> 8, pers (persona +0x6c) -> 9 -> 10,
 *    EA Messenger -> 11 -> 12, the race report -> 13/14.
 *  - UI_OLISPConnect (0x49df20) starts NetConn (0x585640) at stage 0 and sets
 *    stage 2; UI_OLEALogin's constructor (0x4d0738) closes a session left
 *    over (stage > 2) and sets stage 3.  The other screens fill the three
 *    fields and run the stages to 6, 8 and 12.
 *  - UI_OLMAIN (constructor 0x4e5f80, handler 0x4f19a0) runs the stages to 14
 *    with its own "connecting" dialog: the LAN path goes there straight from
 *    its name screen (0x4a2e20: account = persona = password = the name).
 * The override below does what UI_OLISPConnect and UI_OLEALogin's constructor
 * do, fills the fields as the LAN name screen does, and shows UI_OLMAIN; the
 * flow is never built ([0x836370] stays as it was, 0 after any earlier visit:
 * leaving online, 0x4a0140, clears it, as reaching UI_OLMAIN does).  Logoff
 * and Back on the Online Main Menu are the game's own (0x4a0140: stage 0, the
 * main menu).
 * A login that fails on the way shows the game's own dialog on the Online Main
 * Menu (the callbacks 0x4b9090 connect, 0x4b9110 auth, 0x4b9220 pers; a name
 * the server refuses gets "There was a problem selecting this persona.  You
 * will be returned to the EA Login screen.").  UI_OLMAIN has no answer to
 * their OK (it expects a login the screens before it made), so the second
 * override below gives the one the screens would: the name refused (auth or
 * pers: the stage is back at 6 or 8) -> leave online and the EA Login screens,
 * where another name can be typed (UI_OLEALogin also offers "Use <name>": the
 * game's own saved login, from the account field); no connection (stage 4) ->
 * leave online to the main menu, as UI_OLEALogin's Cancel does.
 * A name another player holds the server gives with four digits instead (it
 * refuses a persona only when it finds no free digits for it, or the persona
 * is not the account's): online_lobby_read() below
 * puts that name in the game's own fields as the server's answer arrives.
 */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int winsock_online(void);                    /* winsock.c: online on (browser, NFSU2_ONLINE) */
unsigned roam_plat_name(char out[33]);       /* web/roam_web.c: the name picked on the page while the game runs */

#define G_PROFILE          0x00850080u  /* the online/LAN profile object */
#define G_NETMODE          0x00850078u  /* 8 online, 0x200 LAN */
#define G_STAGE            0x008500d0u  /* +0x50: the login stage */
#define G_ACCOUNT          0x008500d8u  /* +0x58: online account name, 16 bytes */
#define G_PERSONA          0x008500ecu  /* +0x6c: online persona, 16 bytes */
#define G_PASSWORD         0x00850100u  /* +0x80: online password, 16 bytes */
#define G_FLOW             0x00836370u  /* the login screens' flow object */
#define G_PROFILE_NAME     0x0083a9e0u  /* the career profile's name, 16 bytes ("No Profile") */
#define G_NETGAME          0x0089cf48u
#define FN_NETCONN_STARTUP 0x00585640u  /* cdecl () */
#define FN_SET_STAGE       0x005031b0u  /* thiscall profile, (stage) */
#define FN_LOBBY_DROP      0x0058fd50u  /* cdecl (0) */
#define FN_NETGAME_END     0x005fe2b0u  /* thiscall 0x89cf48, (0) */
#define FN_FE_SWITCH       0x00537980u  /* cdecl (fe, package name, 0) */
#define FN_LEAVE_ONLINE    0x004a0140u  /* cdecl (fe or 0): Logoff, then UI_Main unless 0 */
#define S_OLMAIN           0x0078f240u  /* "UI_OLMAIN.fng" */
#define RET_IN_FLOW        0x004f001fu  /* a return address inside 0x4effd0 */
/* the OK of a login error dialog (the callbacks above): pers, a known auth
 * code, and the connection's Cancel (or an unknown auth code) */
#define MSG_PERS_OK        0xaa54bf29u
#define MSG_AUTH_OK        0x904aca43u
#define MSG_CANCEL         0x2c0e485au

static int g_auto;                  /* the Online Main Menu was shown by this file */
static char g_name[16];

/* 3 to 15 of A-Z a-z 0-9 _ -, as the server takes a name (nfsu2-online valid_name):
 * any other run of characters becomes one '_' (none at either end), then cut
 * to 15; one or two characters are padded with '_'.  0 if nothing is left. */
static int clean(char out[16], const char *src){
    int n = 0, gap = 0;
    for (; *src && n < 15; src++){
        char ch = *src;
        int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok){ gap = 1; continue; }
        if (gap && n && n < 14) out[n++] = '_';
        gap = 0;
        out[n++] = ch;
    }
    if (!n){ out[0] = 0; return 0; }
    while (n < 3) out[n++] = '_';
    out[n] = 0;
    return 1;
}

/* the player's name: the one it picked on the page while the game runs (the
 * operator took the old one away: roam_plat_name), else roam_name without a
 * comment after it (roam_game.c's opt(): a ';' or '#' at the start or after a
 * blank), else the profile's */
static int player_name(char out[16], const char **from){
    char picked[33];
    if (roam_plat_name(picked) && clean(out, picked)){ *from = "the name picked on the page"; return 1; }
    const char *v = getenv("NFSU2_ROAM_NAME");
    char buf[64]; int n = 0;
    for (int i = 0; v && v[i] && n < (int)sizeof buf - 1; i++){
        if ((v[i] == ';' || v[i] == '#') && (i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t')) break;
        buf[n++] = v[i];
    }
    buf[n] = 0;
    *from = "roam_name";
    if (clean(out, buf)) return 1;
    if (n) fprintf(stderr, "[online] roam_name \"%s\" has no letters, digits, - or _ for an online name\n", buf);
    if (!mem_is_committed(G_PROFILE_NAME) || !mem_is_committed(G_PROFILE_NAME + 15u)) return 0;
    char pn[17];
    for (n = 0; n < 16 && MR8(G_PROFILE_NAME + (uint32_t)n); n++) pn[n] = (char)MR8(G_PROFILE_NAME + (uint32_t)n);
    pn[n] = 0;
    *from = "the profile's name";
    int language_no_profile(const char *name);           /* runtime/language.c: in the game's language */
    return !language_no_profile(pn) && clean(out, pn);
}

/* a guest call with every register restored afterwards (any convention):
 * by address, or (direct) a lifted function this file overrides */
static void call_to(CPU *c, uint32_t fn, void (*direct)(CPU *), uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(RET_IN_FLOW);
    if (direct) direct(c); else dispatch_indirect(c, fn);
    *c = save;
}
static void call(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){ call_to(c, fn, 0, ecx, n, arg); }

static void field(uint32_t at, const char *s){
    memset(g_mem + at, 0, 16);
    memcpy(g_mem + at, s, strlen(s));
}

/* 0x4effd0(fe), cdecl, from the main menu's Online (above) */
void sub_004effd0_orig(CPU *c);
void sub_004effd0(CPU *c){
    char name[16]; const char *from = "";
    uint32_t fe = MR32(c->esp + 4u), stage = MR32(G_STAGE);
    g_auto = 0;
    if (!winsock_online() || MR32(G_NETMODE) != 8u || stage == 1u){ sub_004effd0_orig(c); return; }
    if (!player_name(name, &from)){
        fprintf(stderr, "[online] no name for the Online menu (set roam_name, or load a profile): the EA Login screens\n");
        sub_004effd0_orig(c); return;
    }
    uint32_t a[3];
    if (stage == 0u){                                      /* UI_OLISPConnect */
        call(c, FN_NETCONN_STARTUP, 0, 0, 0);
        a[0] = 2; call(c, FN_SET_STAGE, G_PROFILE, 1, a);
    } else if (stage > 2u){                                /* UI_OLEALogin's constructor */
        a[0] = 0; call(c, FN_LOBBY_DROP, 0, 1, a);
        a[0] = 0; call(c, FN_NETGAME_END, G_NETGAME, 1, a);
        a[0] = 3; call(c, FN_SET_STAGE, G_PROFILE, 1, a);
    }
    field(G_ACCOUNT, name); field(G_PERSONA, name); field(G_PASSWORD, name);
    fprintf(stderr, "[online] Online menu as %s (%s): no EA Login screens\n", name, from);
    memcpy(g_name, name, sizeof g_name); g_auto = 1;
    a[0] = fe; a[1] = S_OLMAIN; a[2] = 0; call(c, FN_FE_SWITCH, 0, 3, a);
    c->eax = MR32(G_FLOW);
    POP32(c->eip);
}

/* UI_OLMAIN's handler (0x4f19a0, thiscall (msg, 3 more), ret 0x10): its login
 * state is at +0x150 (+4: stopped after an error, 0x4e37d0), its package at +4 */
void sub_004f19a0_orig(CPU *c);
void sub_004f19a0(CPU *c){
    uint32_t self = c->ecx, msg = MR32(c->esp + 4u), stage = MR32(G_STAGE);
    int failed = g_auto && MR32(G_NETMODE) == 8u && stage < 12u && MR8(self + 0x154u) == 1u
                 && (msg == MSG_PERS_OK || msg == MSG_AUTH_OK || msg == MSG_CANCEL);
    sub_004f19a0_orig(c);
    if (!failed) return;
    g_auto = 0;
    uint32_t fe = MR32(self + 4u), a[1];
    if (stage < 6u){
        fprintf(stderr, "[online] the Online menu could not connect: back to the main menu\n");
        a[0] = fe; call(c, FN_LEAVE_ONLINE, 0, 1, a);
        return;
    }
    fprintf(stderr, "[online] the server did not take %s (no free digits for it?): the EA Login screens\n", g_name);
    a[0] = 0; call(c, FN_LEAVE_ONLINE, 0, 1, a);
    a[0] = fe; call_to(c, 0, sub_004effd0_orig, 0, 1, a);
}

/* The persona the server gave: its answer to pers (code 0) names it, the name
 * the game asked for or, when another player is online or racing under that,
 * the name with four digits in place of its end (nfsu2-online free_variant).  The game finds
 * itself among a race's players by its own persona and account fields, so they
 * take the server's name: a game that kept the name it asked for took the
 * other player's place for its own and sent its race traffic to itself.
 * winsock.c hands over the lobby link's bytes as the game reads them; a frame
 * is 12 bytes of header (kind, code, u32 big-endian length, the header
 * included) and TagField text (nfsu2-online aries.rs). */
static int name_char(int ch){ return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'; }
static void persona_given(const char *body){
    const char *v = 0;
    for (const char *p = body; *p; p++)
        if ((p == body || (unsigned char)p[-1] <= 0x20) && !strncasecmp(p, "PERS=", 5)){ v = p + 5; break; }
    if (!v) return;
    if (*v == '"') v++;
    char given[16]; int n = 0;
    while (n < 15 && name_char((unsigned char)v[n])){ given[n] = v[n]; n++; }
    given[n] = 0;
    if (n < 3 || name_char((unsigned char)v[n])) return;
    char asked[17] = "", account[17] = "";
    memcpy(asked, g_mem + G_PERSONA, 16); memcpy(account, g_mem + G_ACCOUNT, 16);
    if (!strcmp(asked, given)) return;
    /* the account too when it is the persona (this file's login, and LAN's): the
     * next login (after a race) asks for the account's own persona */
    if (!strcmp(account, asked)) field(G_ACCOUNT, given);
    field(G_PERSONA, given);
    if (!strcmp(g_name, asked)) memcpy(g_name, given, sizeof g_name);
    fprintf(stderr, "[online] the server gave the name %s (another player is online under %s)\n", given, asked);
}
static uint32_t g_link;              /* the lobby link being read (its connection id) */
static uint8_t g_hdr[12];
static int g_hdr_n, g_keep;          /* header bytes so far; this frame is a pers answer */
static uint32_t g_left;              /* body bytes of this frame still to come */
static char g_body[256];
static int g_body_n;
void online_lobby_read(uint32_t link, const uint8_t *p, int n){
    if (link != g_link){ g_link = link; g_hdr_n = 0; g_left = 0; g_keep = 0; }
    while (n > 0){
        if (!g_left){
            int k = 12 - g_hdr_n < n ? 12 - g_hdr_n : n;
            memcpy(g_hdr + g_hdr_n, p, (size_t)k); g_hdr_n += k; p += k; n -= k;
            if (g_hdr_n < 12) return;
            g_hdr_n = 0;
            uint32_t len = (uint32_t)g_hdr[8] << 24 | (uint32_t)g_hdr[9] << 16 | (uint32_t)g_hdr[10] << 8 | g_hdr[11];
            if (len <= 12) continue;
            g_left = len - 12; g_body_n = 0;
            g_keep = !memcmp(g_hdr, "pers", 4) && !memcmp(g_hdr + 4, "\0\0\0\0", 4) && g_left < sizeof g_body;
            continue;
        }
        int k = (uint32_t)n < g_left ? n : (int)g_left;
        if (g_keep){ memcpy(g_body + g_body_n, p, (size_t)k); g_body_n += k; }
        p += k; n -= k; g_left -= (uint32_t)k;
        if (!g_left && g_keep){ g_body[g_body_n] = 0; g_keep = 0; persona_given(g_body); }
    }
}
