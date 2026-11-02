/* olfilter.c -- the Online lobby's Filter narrows its game list once the
 * player has set it.
 *
 * The online host lists every open game, those the Filter asks for first
 * (web/server-rs/crates/online lobby.rs game_search, since 0b92d3f): nobody
 * changed the game's default Filter, so a game of other settings was seen by
 * no one.  A player who did set it saw the other games all the same ("the
 * filters don't work either", a player's report, 2026-10-23).
 *
 * The game keeps the Filter in the network race's settings (0x850080: race
 * type [0x850088], car restrictions [0x85008c], ranked [0x850098], Race
 * Breaker [0x850099], Collision Detection [0x85009a]): the ones Create Game's
 * screens set, and a join copies those of the game it joined into them
 * (0x58b070).  The lobby's search (0x4bb0f0 -> 0x5904c0, gsea) sends them as
 * CUSTFLAGS and SYSFLAGS under their masks, so the host cannot tell a Filter
 * the player set from the settings of a game they made or joined.
 *
 * Here: leaving the Filter screen (UI_OLFilters.fng, handler 0x4a0c30, only
 * from its vtable 0x796200: Apply Filter 0xc407210 / 0x406415e3, or Back
 * 0x911ab364; both go back to the lobby with the settings as the screen left
 * them) notes those settings.  A gsea to the online host while the settings
 * are still those carries the code "filt" (the header's [4..7], 0 in the
 * game's own requests), and the host lists only the games such a search asks
 * for, unless it asks for the Filter's defaults (Circuit, no restriction,
 * both options on, unranked).  A game made or joined since changes the
 * settings: the list is the whole one again until the next visit to the
 * Filter screen.  Original: EA's server took every search as a Filter. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>

#define FN_FILTER_MSG   0x004a0c30u   /* thiscall (msg, obj, a3, a4), ret 0x10 */
#define MSG_APPLY       0x0c407210u
#define MSG_ACCEPT      0x406415e3u
#define MSG_BACK        0x911ab364u
#define G_MODE          0x00850088u   /* dword: the race type */
#define G_RESTRICT      0x0085008cu   /* byte: car restrictions (1: none) */
#define G_OPTIONS       0x00850098u   /* bytes: ranked, Race Breaker, Collision Detection */

void sub_004a0c30(CPU *c);
void dispatch_patch(uint32_t a, void (*f)(CPU *));

static int g_set;                     /* the player left the Filter screen with these settings */
static uint8_t g_filter[8];

static void settings(uint8_t s[8]){
    uint32_t m = MR32(G_MODE);
    memcpy(s, &m, 4);
    s[4] = MR8(G_RESTRICT); s[5] = MR8(G_OPTIONS); s[6] = MR8(G_OPTIONS + 1u); s[7] = MR8(G_OPTIONS + 2u);
}
static int filter_on(void){
    uint8_t s[8];
    settings(s);
    return g_set && !memcmp(s, g_filter, sizeof s);
}

static void filter_msg(CPU *c){
    uint32_t m = MR32(c->esp + 4u);
    sub_004a0c30(c);
    if (m != MSG_APPLY && m != MSG_ACCEPT && m != MSG_BACK) return;
    settings(g_filter); g_set = 1;
    fprintf(stderr, "[online] the Filter: race type %u, restrictions %u, ranked %u, Race Breaker %u, collisions %u\n",
            MR32(G_MODE), g_filter[4], g_filter[5], g_filter[6], g_filter[7]);
}

void olfilter_install(void){ dispatch_patch(FN_FILTER_MSG, filter_msg); }

/* The bytes the game sends the online host's lobby (winsock.c), a message
 * after another: [0..3] kind, [4..7] code, [8..11] big-endian length with
 * these 12.  olfilter_lobby_mark() puts a code in the message's header
 * within the bytes about to go (the game's own buffer), olfilter_lobby_sent()
 * moves on by the bytes that went: a send cut short marks the rest when it
 * goes.  The codes: "filt" in a gsea (above), and in a rank (the race's
 * report) "ed" and our car's type when its game data is edited
 * (mprace_rank_code: the host's [cheat] lines). */
typedef struct { uint32_t link, at, len; uint8_t hdr[12]; int mark; char code[4]; } Stream;
static Stream g_out;

int mprace_rank_code(char code[4]);   /* mprace.c */
static int code_for(Stream *st){
    if (!memcmp(st->hdr, "gsea", 4) && filter_on()){ memcpy(st->code, "filt", 4); return 1; }
    return !memcmp(st->hdr, "rank", 4) && mprace_rank_code(st->code);
}

static void walk(Stream *st, uint8_t *p, int n, int patch){
    for (int i = 0; i < n;){
        if (st->at < 12u){
            if (st->at == 4u) st->mark = patch && code_for(st);
            if (st->at >= 4u && st->at < 8u && st->mark && !p[i]) p[i] = (uint8_t)st->code[st->at - 4u];
            st->hdr[st->at++] = p[i++];
            if (st->at == 12u){
                st->len = (uint32_t)st->hdr[8] << 24 | (uint32_t)st->hdr[9] << 16 | (uint32_t)st->hdr[10] << 8 | st->hdr[11];
                if (st->len <= 12u) st->at = 0;
            }
            continue;
        }
        uint32_t k = st->len - st->at;
        if (k > (uint32_t)(n - i)) k = (uint32_t)(n - i);
        i += (int)k; st->at += k;
        if (st->at >= st->len) st->at = 0;
    }
}
void olfilter_lobby_mark(uint32_t link, uint8_t *p, int n){
    if (link != g_out.link){ memset(&g_out, 0, sizeof g_out); g_out.link = link; }
    Stream st = g_out;
    walk(&st, p, n, 1);
}
void olfilter_lobby_sent(uint32_t link, uint8_t *p, int n){
    if (link != g_out.link){ memset(&g_out, 0, sizeof g_out); g_out.link = link; }
    walk(&g_out, p, n, 0);
}
