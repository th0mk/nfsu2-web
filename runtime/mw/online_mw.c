/* online_mw.c -- Most Wanted's Online menu logs in under the player's name, as
 * runtime/online.c does for Underground 2.
 *
 * The browser's server is its own stand-in for EA's servers (web/server-rs/
 * crates/online, title.rs MW; runtime/winsock.c resolves pcnfs06.ea.com to
 * it): any name logs in, there are no accounts, so the EA Login screens
 * (OLEALogin.fng, OL_Create_Account*, OL_UseExisting, OL_SelectPersona) ask
 * for nothing the server needs.  With online on, main menu -> Online logs in
 * as the player's name, roam_name (NFSU2_ROAM_NAME, the name friends see in
 * free roam), the way the LAN screens log in with the name typed there.
 * Without a usable name, or with online off, the game's own screens come.
 *
 * The front end (speed.exe v1.3, static reading, GAME.md 12):
 *  - The main menu (MainMenu.fng, handler 0x559FC0) keeps the mode chosen in
 *    the bits of [[0x91CF90]+0x12C]: bit 3 Online, bit 6 LAN.  Online ends in
 *    its message 0xC6C6B68F: 0x78AE80 (the online heap) and 0x5538E0(fe).
 *  - 0x5538E0(fe), cdecl, makes the login flow (0x164 bytes, constructor
 *    0x5496A0, kept at [0x91CA68]) from UI_OLISPConnect.fng to OL_MAIN.fng
 *    (back: MainMenu.fng).  The LAN screens call it too (0x55914C, 0x55926E),
 *    after their name screen set the name.
 *  - Its tick 0x553980 runs the login by the net state (0x78B380): 1 connect
 *    (0x792BF0: the host 0x56E440, port 0x56E460, 60 s), 2 auth (0x792CA0:
 *    account, password), 5 persona (0x792EF0), 6 the rest (EA Messenger, the
 *    Online main menu).  Online, a flow whose +4 is -1 (the constructor's)
 *    goes to the EA Login screens at 1 and 2 (0x533560) to fill the fields;
 *    LAN sets +4 to 2 and logs in with what the name screen set.
 *  - The fields: the login record L = [0x91CF90]+0x134, Online's account
 *    L+0x70, persona L+0x84, password L+0x98 (LAN's at L+0xAC, +0xC0, +0xD4),
 *    16 characters each (the setters 0x56E380, 0x56E3C0, 0x56E400).
 * The override below lets the main menu's flow be made, then fills the three
 * Online fields with the name, as the LAN name screen fills LAN's (0x559090),
 * and sets the flow's +4 to 2: the flow connects, logs in and picks that
 * persona by itself.  A name the server refuses shows the game's own error
 * dialog (the flow's error handling, 0x553D64).
 * A name another player holds the server gives with four digits instead, at
 * the login already (nfsu2-online free_variant, title.rs name_at_auth):
 * online_lobby_read() below puts that name in the game's own account and
 * persona fields as the server's answer arrives. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int winsock_online(void);                    /* winsock.c: online on (browser, NFSU2_ONLINE) */

#define G_ONLINE      0x0091cf90u   /* the online state object (a pointer) */
#define ON_MODE       0x12cu        /* its mode bits: 3 Online, 6 LAN */
#define ON_LOGIN      0x134u        /* its login record L */
#define L_ACCOUNT     0x70u         /* Online's account, persona, password: 16 characters each */
#define L_PERSONA     0x84u
#define L_PASSWORD    0x98u
#define G_FLOW        0x0091ca68u   /* the login flow (0x5538E0) */
#define FLOW_LOGIN    4u            /* -1: the EA Login screens fill the fields; 2: log in with them */

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

/* the player's name: roam_name without a comment after it (a ';' or '#' at the
 * start or after a blank, as roam_game.c's opt() reads it) */
static int player_name(char out[16]){
    const char *v = getenv("NFSU2_ROAM_NAME");
    char buf[64]; int n = 0;
    for (int i = 0; v && v[i] && n < (int)sizeof buf - 1; i++){
        if ((v[i] == ';' || v[i] == '#') && (i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t')) break;
        buf[n++] = v[i];
    }
    buf[n] = 0;
    if (clean(out, buf)) return 1;
    if (n) fprintf(stderr, "[online] roam_name \"%s\" has no letters, digits, - or _ for an online name\n", buf);
    return 0;
}

static void field(uint32_t at, const char *s){
    memset(g_mem + at, 0, 16);
    memcpy(g_mem + at, s, strlen(s));
}

/* the online state object, or 0 while there is none */
static uint32_t online_state(void){
    uint32_t o = MR32(G_ONLINE);
    return o && mem_is_committed(o) && mem_is_committed(o + ON_LOGIN + L_PASSWORD + 16u) ? o : 0;
}

/* 0x5538E0(fe), cdecl: the login flow (see the top) */
void sub_005538e0_orig(CPU *c);
void sub_005538e0(CPU *c){
    sub_005538e0_orig(c);
    uint32_t o = online_state(), flow = MR32(G_FLOW);
    if (!winsock_online() || !o || !flow || !mem_is_committed(flow + FLOW_LOGIN)) return;
    uint32_t mode = MR32(o + ON_MODE);
    if (!(mode & 8u) || (mode & 0x40u)) return;            /* Online only, never LAN */
    char name[16];
    if (!player_name(name)){
        fprintf(stderr, "[online] no name for the Online menu (set roam_name): the EA Login screens\n");
        return;
    }
    uint32_t l = o + ON_LOGIN;
    field(l + L_ACCOUNT, name); field(l + L_PERSONA, name); field(l + L_PASSWORD, name);
    MR32(flow + FLOW_LOGIN) = 2u;
    memcpy(g_name, name, sizeof g_name);
    fprintf(stderr, "[online] Online menu as %s (roam_name): no EA Login screens\n", name);
}

/* The name the server gave.  Its answer to auth (code 0) names the account,
 * its answer to pers the persona: the name the game asked for or, when another
 * player is online or racing under it, the name with four digits in place of
 * its end (nfsu2-online free_variant).  For Most Wanted the server gives those
 * digits at auth already (title.rs name_at_auth): the game's lobby library
 * keeps the name it logged in with (three copies of it in its heap stayed the
 * asked name after a persona given at pers, and that joiner left its game room
 * before the race: a run of 2026-10-30).  The game's account and persona
 * fields take the given name before the game asks for the persona, so every
 * copy the library makes from then on is the given one.  winsock.c hands over
 * the lobby link's bytes as the game reads them; a frame is 12 bytes of header
 * (kind, code, u32 big-endian length, the header included) and TagField text
 * (nfsu2-online aries.rs). */
static int name_char(int ch){ return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'; }
static void name_given(const char *body, const char *tag, int is_auth){
    uint32_t o = online_state();
    if (!o || (MR32(o + ON_MODE) & 0x40u)) return;         /* LAN: EA's own server.dll answers */
    size_t tl = strlen(tag);
    const char *v = 0;
    for (const char *p = body; *p; p++)
        if ((p == body || (unsigned char)p[-1] <= 0x20) && !strncasecmp(p, tag, tl) && p[tl] == '='){ v = p + tl + 1; break; }
    if (!v) return;
    if (*v == '"') v++;
    char given[16]; int n = 0;
    while (n < 15 && name_char((unsigned char)v[n])){ given[n] = v[n]; n++; }
    given[n] = 0;
    if (n < 3 || name_char((unsigned char)v[n])) return;
    uint32_t l = o + ON_LOGIN;
    char persona[17] = "", account[17] = "";
    memcpy(persona, g_mem + l + L_PERSONA, 16); memcpy(account, g_mem + l + L_ACCOUNT, 16);
    const char *asked = is_auth ? account : persona;
    if (!strcmp(asked, given)) return;
    char was[17]; memcpy(was, asked, sizeof was);
    /* the account and the persona when they are one name (this file's login):
     * a later login asks for the account's own persona */
    if (is_auth || !strcmp(account, persona)){ field(l + L_ACCOUNT, given); field(l + L_PERSONA, given); }
    else field(l + L_PERSONA, given);
    if (!strcmp(g_name, was)) memcpy(g_name, given, sizeof g_name);
    fprintf(stderr, "[online] the server gave the name %s at %s (another player is online under %s)\n", given,
            is_auth ? "the login" : "the persona", was);
}
static uint32_t g_link;              /* the lobby link being read (its connection id) */
static uint8_t g_hdr[12];
static int g_hdr_n, g_keep;          /* header bytes so far; this frame is an answer to keep: 1 pers, 2 auth */
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
            g_keep = memcmp(g_hdr + 4, "\0\0\0\0", 4) || g_left >= sizeof g_body ? 0
                   : !memcmp(g_hdr, "pers", 4) ? 1 : !memcmp(g_hdr, "auth", 4) ? 2 : 0;
            continue;
        }
        int k = (uint32_t)n < g_left ? n : (int)g_left;
        if (g_keep){ memcpy(g_body + g_body_n, p, (size_t)k); g_body_n += k; }
        p += k; n -= k; g_left -= (uint32_t)k;
        if (!g_left && g_keep){
            g_body[g_body_n] = 0;
            if (g_keep == 2) name_given(g_body, "NAME", 1); else name_given(g_body, "PERS", 0);
            g_keep = 0;
        }
    }
}
