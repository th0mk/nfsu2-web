/* roam_hubwire.c -- the C side of the hub's tests (web/server-rs/crates/roam/tests/roam_hub.rs, roam_ahead.rs): what
 * runtime/roam_core.c sends the server's free-roam hub, and what it makes of
 * the hub's answer.
 *   roam_hubwire hello|snap|bye   one datagram, as roam_core encodes it, in hex
 *   roam_hubwire peers <hex>      a PEERS datagram decoded: "ok <n> a.b.c.d:port:id ..." or "err <code>"
 *   roam_hubwire map <hex>...     a session that hears the hub (10.77.0.253:47210) asks for the world map
 *                                 ("ask <hex>": its MAPREQ) and takes these datagrams from the hub (each "ask"
 *                                 among them: it asks again, a second on); then "map <gen> <total> <n>
 *                                 id:x,y,h,vx,vy ..." (what it holds)
 *   roam_hubwire lobby            a race lobby's advert to the hub (to 0), as roam_core encodes it, in hex
 *   roam_hubwire lobbies <hex>... a session that takes race lobbies asks for the world map as above and takes
 *                                 these datagrams; then "lobbies <gen> <n> x,y,race,n,max,state ..."
 *   roam_hubwire racerep          a race's report to the hub, as roam_core encodes it, in hex
 *   roam_hubwire hellocrew        a session's HELLO to the hub, in hex: its crews "Blue Room" and "Night Owls"
 *                                 (rc_room_hash) after the ids of the two players the hub listed, who said hello
 *   roam_hubwire hubcrew <hex>    a session in those two crews takes this list from the hub; then each player
 *                                 listed as "a.b.c.d:port:crew" (rc_peer_crew: 0 or 1, -1 for none) */
#include "roam_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void unhex(const char *h, uint8_t *b, int *n){
    *n = 0;
    for (; h[0] && h[1] && *n < RC_MAX_DGRAM; h += 2){ unsigned v; sscanf(h, "%2x", &v); b[(*n)++] = (uint8_t)v; }
}
static void sent(void *user, uint32_t ip, uint16_t port, const uint8_t *buf, int len){
    (void)user;
    if (ip != 0x0A4D00FDu || port != 47210 || len < 6 || buf[5] != RC_MSG_MAPREQ) return;
    printf("ask ");
    for (int i = 0; i < len; i++) printf("%02x", buf[i]);
    printf("\n");
}
/* the last HELLO a session sent the hub (hellocrew) */
static uint8_t hub_hello[RC_MAX_DGRAM];
static int hub_hello_n;
static void sent_hello(void *user, uint32_t ip, uint16_t port, const uint8_t *buf, int len){
    (void)user;
    if (ip != 0x0A4D00FDu || port != 47210 || len < 6 || len > RC_MAX_DGRAM || buf[5] != RC_MSG_HELLO) return;
    memcpy(hub_hello, buf, (size_t)len); hub_hello_n = len;
}
static rc_session S;

int main(int argc, char **argv){
    if (argc < 2) return 2;
    rc_msg m; memset(&m, 0, sizeof m);
    m.h.sender = 0x1234ABCDu; m.h.seq = 7;
    uint8_t b[RC_MAX_DGRAM]; int n;
    if (!strcmp(argv[1], "hellocrew")){
        /* a session in free roam with a hub and two crews; the hub lists two players, who say hello (they hear
         * us); the session's next HELLO to the hub (rc_tick's, fill_hello) names their ids, then its crews */
        rc_config c; memset(&c, 0, sizeof c);
        c.self_id = 0x1234ABCDu; c.port = 47210; c.max_players = RC_MAX_PLAYERS; c.rate_hz = 20; strcpy(c.name, "Ann K.");
        c.hub_ip = 0x0A4D00FDu; c.hub_port = 47210;
        c.crews[0] = rc_room_hash("Blue Room"); c.crews[1] = rc_room_hash("Night Owls"); c.n_crews = 2;
        rc_io io; memset(&io, 0, sizeof io); io.send = sent_hello;
        rc_session_init(&S, &c, &io);
        int64_t t = 1000000;
        rc_set_status(&S, RC_ST_ROAM);
        rc_tick(&S, t);
        const uint32_t ip[2] = { 0x0A4D0011u, 0x0A4D0022u }, id[2] = { 0x11u, 0x22u };
        m.h.type = RC_MSG_PEERS; m.h.sender = 0xB0Bu; m.u.peers.n = 2;
        for (int k = 0; k < 2; k++){ m.u.peers.e[k].ip = ip[k]; m.u.peers.e[k].port = 47210; m.u.peers.e[k].id = id[k]; }
        n = rc_encode(&m, b, sizeof b);
        rc_receive(&S, 0x0A4D00FDu, 47210, b, n, t += 1000);
        for (int k = 0; k < 2; k++){
            rc_msg h; memset(&h, 0, sizeof h);
            h.h.type = RC_MSG_HELLO; h.h.sender = id[k]; h.h.seq = 1; h.h.flags = RC_HF_STATEX | RC_HF_CREW;
            h.u.hello.core_ver = RC_CORE_VER; h.u.hello.platform = RC_PLAT_MAC; h.u.hello.status = RC_ST_ROAM;
            h.u.hello.listen_port = 47210; h.u.hello.cfg_rev = 0; snprintf(h.u.hello.name, sizeof h.u.hello.name, "P%d", k);
            h.u.hello.n_seen = 1; h.u.hello.seen[0] = c.self_id;
            n = rc_encode(&h, b, sizeof b);
            rc_receive(&S, ip[k], 47210, b, n, t += 1000);
        }
        hub_hello_n = 0;
        rc_tick(&S, t += 1500000);
        if (!hub_hello_n){ printf("none\n"); return 1; }
        for (int i = 0; i < hub_hello_n; i++) printf("%02x", hub_hello[i]);
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "hubcrew")){
        rc_config c; memset(&c, 0, sizeof c);
        c.self_id = 0x1234ABCDu; c.port = 47210; c.max_players = RC_MAX_PLAYERS; c.rate_hz = 20;
        c.hub_ip = 0x0A4D00FDu; c.hub_port = 47210;
        c.crews[0] = rc_room_hash("Blue Room"); c.crews[1] = rc_room_hash("Night Owls"); c.n_crews = 2;
        rc_io io; memset(&io, 0, sizeof io);
        rc_session_init(&S, &c, &io);
        int64_t t = 1000000;
        rc_tick(&S, t);
        unhex(argc > 2 ? argv[2] : "", b, &n);
        if (rc_decode(b, n, &m) != RC_OK || m.h.type != RC_MSG_PEERS){ printf("err\n"); return 0; }
        rc_receive(&S, 0x0A4D00FDu, 47210, b, n, t += 1000);
        printf("ok");
        for (int k = 0; k < m.u.peers.n; k++){
            uint32_t ip = m.u.peers.e[k].ip; int crew = -2;
            for (int i = 0; i < RC_TABLE; i++)
                if (S.peer[i].state != RC_PS_FREE && S.peer[i].ip == ip && S.peer[i].port == m.u.peers.e[k].port) crew = rc_peer_crew(&S, i);
            printf(" %u.%u.%u.%u:%u:%d", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u, m.u.peers.e[k].port, crew);
        }
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "map") || !strcmp(argv[1], "lobbies")){
        rc_config c; memset(&c, 0, sizeof c);
        c.self_id = 0x1234ABCDu; c.port = 47210; c.max_players = RC_MAX_PLAYERS; c.rate_hz = 20;
        c.hub_ip = 0x0A4D00FDu; c.hub_port = 47210; c.lobbies = !strcmp(argv[1], "lobbies");
        rc_io io; memset(&io, 0, sizeof io); io.send = sent;
        rc_session_init(&S, &c, &io);
        int64_t t = 1000000;
        rc_tick(&S, t);
        m.h.type = RC_MSG_PEERS; m.h.sender = 0xB0Bu;                 /* the hub answers: an empty list */
        n = rc_encode(&m, b, sizeof b);
        rc_receive(&S, 0x0A4D00FDu, 47210, b, n, t += 1000);
        if (!rc_ask_map(&S, t += 1000, RC_MAP_MAX)){ printf("not asked\n"); return 0; }
        for (int i = 2; i < argc; i++){
            if (!strcmp(argv[i], "ask")){ if (!rc_ask_map(&S, t += 1000000, RC_MAP_MAX)){ printf("not asked\n"); return 0; } continue; }
            unhex(argv[i], b, &n); rc_receive(&S, 0x0A4D00FDu, 47210, b, n, t += 1000);
        }
        if (c.lobbies){
            printf("lobbies %u %d", S.lobbies_gen, S.lobbies_n);
            for (int i = 0; i < S.lobbies_n; i++) printf(" %d,%d,%u,%u,%u,%u", S.lobbies_e[i].x, S.lobbies_e[i].y, S.lobbies_e[i].race,
                                                     S.lobbies_e[i].n, S.lobbies_e[i].max, S.lobbies_e[i].state);
            printf("\n");
            return 0;
        }
        printf("map %u %u %d", S.map_gen, S.map_total, S.map_n);
        for (int i = 0; i < S.map_n; i++)
            printf(" %u:%d,%d,%u,%d,%d", S.map_e[i].id, S.map_e[i].x, S.map_e[i].y, S.map_e[i].heading, S.map_e[i].vx, S.map_e[i].vy);
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "peers")){
        unhex(argc > 2 ? argv[2] : "", b, &n);
        int e = rc_decode(b, n, &m);
        if (e != RC_OK || m.h.type != RC_MSG_PEERS){ printf("err %d\n", e); return 0; }
        printf("ok %u", m.u.peers.n);
        for (int i = 0; i < m.u.peers.n; i++){
            uint32_t ip = m.u.peers.e[i].ip;
            printf(" %u.%u.%u.%u:%u:%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u, m.u.peers.e[i].port, m.u.peers.e[i].id);
        }
        printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "hello")){
        m.h.type = RC_MSG_HELLO;
        m.u.hello.core_ver = RC_CORE_VER; m.u.hello.platform = RC_PLAT_MAC; m.u.hello.status = RC_ST_ROAM;
        m.u.hello.listen_port = 47210; m.u.hello.cfg_rev = 3; m.u.hello.room = rc_fnv1a("blue");
        strcpy(m.u.hello.name, "Ann K.");
        m.u.hello.n_seen = 2; m.u.hello.seen[0] = 0x11; m.u.hello.seen[1] = 0x22;
    } else if (!strcmp(argv[1], "snap")){
        m.h.type = RC_MSG_SNAP;
        m.u.snap.status = RC_ST_ROAM; m.u.snap.epoch = 1; m.u.snap.cfg_rev = 3; m.u.snap.n = 3;
        for (int k = 0; k < 3; k++){
            rc_state *s = &m.u.snap.s[k];
            s->t_us = 1000000u - 50000u * (uint32_t)k; s->q[3] = 1;
            s->p[0] = 1695.25f - (float)k; s->p[1] = -883.5f; s->p[2] = 24.125f; s->v[0] = 20.f;
        }
    } else if (!strcmp(argv[1], "lobby")){
        m.h.type = RC_MSG_LOBBY;
        rc_lobby *l = &m.u.lobby;
        l->op = RC_LOBBY_ADVERT; l->state = RC_LS_OPEN; l->max = 6; l->to = 0; l->lobby = 0xA1B2C3D4u; l->race = 0x0BADF00Du;
        l->x = -1234; l->y = 567; l->n = 2; l->away_s = 25; l->ids[0] = 0x1234ABCDu; l->ids[1] = 0x55u;   /* (away: the hub skips it) */
    } else if (!strcmp(argv[1], "racerep")){
        m.h.type = RC_MSG_RACEREP;
        rc_racerep *r = &m.u.racerep;
        r->lobby = 0xA1B2C3D4u; r->race = 0x0BADF00Du; r->mode = 1; r->laps = 1; r->track = 4102; r->place = 2; r->players = 3;
        r->flags = RC_RR_HOST; r->type = 22; r->cls = 2; r->rating = 316.6f; r->field = 280.5f; r->factor = 1.25f;
        r->kw = 214.f; r->kg = 1450.f; r->kmh = 249.f; r->time = 95.5f; strcpy(r->name, "350Z");
    } else if (!strcmp(argv[1], "bye")){
        m.h.type = RC_MSG_BYE; m.u.bye.reason = 0;
    } else return 2;
    n = rc_encode(&m, b, sizeof b);
    if (n <= 0) return 1;
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
    return 0;
}
