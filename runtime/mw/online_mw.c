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
#include "widescreen.h"               /* HOOK_SITE */

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

/* ---- the race link's state, for runtime/web/net_lan.c's race log (NFSU2_VLAN_DEBUG=2) ----
 * DirtySock's ConnApi (ref [0x9B9D14], 0x79F090's singleton; clients from +0x198, 0xB0 bytes each, the count at
 * +0x190) checks each active client's NetGameLink (client +0x9C) on the lobby thread (0x85C260 from ConnApiUpdate
 * 0x85D110): no datagram taken in for its timeout [ref+0xDC] (15000 ms by default) drops the link.  NetGameLink takes
 * a datagram in (0x83EF10, which stamps +0x44 with the tick) only with 0x21B bytes free in its input buffer (used
 * +0x198 of +0x19C), which the game empties (0x83F690 from 0x78B120 and the room's 0x7A1640).  Any thread. */
uint32_t host_ms(void);                 /* win32.c: the exe's GetTickCount, NetTick's clock */
int mw_link_probe(char *b, int n){
    uint32_t ref = MR32(0x009b9d14u);
    if (!ref || !mem_is_committed(ref) || !mem_is_committed(ref + 0x198u)) return 0;
    uint32_t cnt = MR32(ref + 0x190u), tmo = MR32(ref + 0xdcu), now = host_ms();
    int o = snprintf(b, (size_t)n, " | tick %u tmo %u room %d", now, tmo, (int)MR32(0x009b4220u));
    for (uint32_t i = 0; i < cnt && i < 8 && o < n - 64; i++){
        uint32_t c = ref + 0x198u + 0xb0u * i;
        if (!mem_is_committed(c + 0xa0u)) break;
        uint32_t st = MR32(c + 0x88u), l = MR32(c + 0x9cu);
        if (!l || !mem_is_committed(l) || !mem_is_committed(l + 0x1a0u)){ o += snprintf(b + o, (size_t)(n - o), " c%u st %u", i, st); continue; }
        uint32_t cm = MR32(l), cs = cm && mem_is_committed(cm + 0x98u) ? MR32(cm + 0x94u) : 99u;
        o += snprintf(b + o, (size_t)(n - o), " c%u st %u cs %u buf %u/%u rcvd -%d", i, st, cs, MR32(l + 0x198u),
                      MR32(l + 0x19cu), (int)(now - MR32(l + 0x44u)));
    }
    return o;
}

/* ---- why ConnApi drops a peer (a log line each time) ----
 * The three calls of its disconnect 0x85C0A0 in the per-client update 0x85C260 (ebp the client, edi the ConnApi
 * ref): the game stopped the connections (bit 0 of [ref+0x2C] clear), a connect that took over 10 s (state 1,
 * tick - [client+0xA4] > 10000), and an active link (state 3) whose NetGameLink is no longer open ([link+0x79], the
 * CommUDP state 2-4) or took nothing in for the timeout ([ref+0xDC]; tick [link+0x24] - last receive [link+0x44]). */
#define CONNAPI_STOPPED   HOOK_SITE(0x0085c2a2)
#define CONNAPI_SLOW      HOOK_SITE(0x0085c43e)
#define CONNAPI_LINK_DOWN HOOK_SITE(0x0085c4a1)
/* CommUDP's own timeout in its update 0x8505E0 (ebp the CommUDP ref, esi the tick): an open link (state 4) whose last
 * datagram came in over 120 s before (tick - [ref+0xE8], unsigned: a datagram stamped after the tick is 2^32 ms) and
 * that sent within 2 s goes to 0x84FDA0, which sends DISC and closes it (state 5); ConnApi then drops the peer */
#define COMMUDP_TIMEOUT   HOOK_SITE(0x008509eb)

/* ---- a LAN race of a type no race has (TRACKER M8-13) ----
 * The race of a LAN game comes from 0x512F90 (thiscall; its argument points at the online race type
 * [[0x91CF90]+0x168]; called from 0x535970): it gathers the races of the race list [0x91E004] (count 0x5DC920, race i
 * 0x5DC930) that pass 0x5FD160(1), 0x576A20(params) and 0x576790(7, params, 0), the params being 0x5FAB40's, and whose
 * race type (0x5FAA20) is the one asked for (0xB: any of 0, 1 and 2), takes one at random (0x45DA30(n)), reads its
 * params and writes its type back.  With none gathered it reads the first slot of the array it allocated and never
 * filled, a wild pointer, and the game dies in 0x5FAB40 (run t6 of 2026-10-31: online type 6 poked on both sides, the
 * host's game trapped 0.25 s after it created the game).  The stock LAN menu offers types 0, 1 and 2 only (GAME 12.3),
 * but a type from anywhere else that no race has is made 0xB here, with a log line; NFSU2_TEST_RACE_TYPE_ANY=1 (test
 * switch, L-063) leaves it as it is, and the crash with it. */
#define RACE_PICK         HOOK_SITE(0x00512f90)
uint32_t mw_aid_call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args);   /* career_mw.c */
static int races_of_type(CPU *c, uint32_t type){
    uint32_t db = MR32(0x0091e004u);
    if (!db || !mem_is_committed(db)) return -1;
    uint32_t n = mw_aid_call(c, 0x005dc920u, db, 0, 0), k = 0;
    for (uint32_t i = 0; i < n && i < 4096u; i++){
        uint32_t r = mw_aid_call(c, 0x005dc930u, db, 1, &i), one = 1;
        if (!r || !(mw_aid_call(c, 0x005fd160u, r, 1, &one) & 0xffu)) continue;
        uint32_t p = mw_aid_call(c, 0x005fab40u, r, 0, 0);
        if (!(mw_aid_call(c, 0x00576a20u, 0, 1, &p) & 0xffu)) continue;
        uint32_t a[3] = { 7u, mw_aid_call(c, 0x005fab40u, r, 0, 0), 0u };
        if (!(mw_aid_call(c, 0x00576790u, 0, 3, a) & 0xffu)) continue;
        uint32_t t = mw_aid_call(c, 0x005faa20u, r, 0, 0);
        if (type == 0xbu ? t <= 2u : t == type) k++;
    }
    return (int)k;
}
static void race_pick(CPU *c){
    static int keep = -1;
    if (keep < 0){ const char *e = getenv("NFSU2_TEST_RACE_TYPE_ANY"); keep = e && *e == '1'; }
    uint32_t pt = MR32(c->esp + 4u);
    if (!pt || !mem_is_committed(pt + 3u)) return;
    uint32_t type = MR32(pt);
    if (type == 0xbu || type <= 2u) return;            /* the menu's own: the game has races of these */
    int n = races_of_type(c, type);
    if (n != 0) return;
    if (keep){
        fprintf(stderr, "[lan] no race of type %u to pick: left so (test switch NFSU2_TEST_RACE_TYPE_ANY), the game reads "
                "an empty slot\n", type);
        return;
    }
    fprintf(stderr, "[lan] no race of type %u to pick: any of types 0, 1 and 2 instead (%d races)\n", type,
            races_of_type(c, 0xbu));
    MR32(pt) = 0xbu;
}

int mw_online_hook(CPU *c, uint32_t va){
    if (va == RACE_PICK){ race_pick(c); return 1; }
    if (va == COMMUDP_TIMEOUT){
        uint32_t r = c->ebp, last = mem_is_committed(r + 0xecu) ? MR32(r + 0xe8u) : 0u, sent = MR32(r + 0xe4u);
        fprintf(stderr, "[net] CommUDP closes its link: no datagram for %u ms (tick %u, last datagram %u%s, last send %u; "
                "the datagram in hand: stamp %u, seq 0x%x ack 0x%x, %d bytes)\n", c->esi - last, c->esi, last,
                (int32_t)(c->esi - last) < 0 ? ": stamped after the tick" : "", sent, MR32(0x009c3ab4u), MR32(0x009c3ab8u),
                MR32(0x009c3abcu), (int)MR32(0x009c3ab0u));
        return 1;
    }
    if (va != CONNAPI_STOPPED && va != CONNAPI_SLOW && va != CONNAPI_LINK_DOWN) return 0;
    uint32_t cl = c->ebp, ref = c->edi, idx = ref && cl >= ref + 0x198u ? (cl - ref - 0x198u) / 0xb0u : 99u;
    uint32_t link = mem_is_committed(cl + 0xa0u) ? MR32(cl + 0x9cu) : 0u;
    if (va == CONNAPI_STOPPED || !link || !mem_is_committed(link + 0x80u)){
        fprintf(stderr, "[net] ConnApi drops peer %u: %s (state %u)\n", idx,
                va == CONNAPI_STOPPED ? "the game stopped its connections" : va == CONNAPI_SLOW ? "no connection in 10 s"
                : "no link", mem_is_committed(cl + 0x8cu) ? MR32(cl + 0x88u) : 0u);
        return 1;
    }
    uint32_t comm = MR32(link), cst = comm && mem_is_committed(comm + 0x98u) ? MR32(comm + 0x94u) : 99u;
    fprintf(stderr, "[net] ConnApi drops peer %u: %s; link open %u, CommUDP state %u, tick %u, last receive %d ms "
            "before, timeout %u ms, input buffer %u of %u\n", idx, va == CONNAPI_SLOW ? "no connection in 10 s"
            : "the link is down", MR8(link + 0x79u), cst, MR32(link + 0x24u), (int)(MR32(link + 0x24u) - MR32(link + 0x44u)),
            mem_is_committed(ref + 0xe0u) ? MR32(ref + 0xdcu) : 0u, MR32(link + 0x198u), MR32(link + 0x19cu));
    return 1;
}
