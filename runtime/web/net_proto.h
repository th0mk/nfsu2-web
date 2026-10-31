/* net_proto.h -- the net mailbox: datagrams between the C runtime and the
 * page's virtual-LAN socket (web/src/net), in shared wasm memory.
 *
 * The C side allocates one mailbox and publishes it with
 * nfsu2_register(HOST_KIND_NET, ptr, NET_MAILBOX_BYTES) (host_abi.h).  The page
 * side is web/src/net/proto.ts (same numbers; web/test/net-proto.test.mjs
 * checks that they agree).
 *
 *   header (512 bytes)   fixed fields, then C -> page words, then page -> C words
 *   TX ring              NET_TX_SLOTS slots, C produces, the page consumes and sends
 *   RX ring              NET_RX_SLOTS slots, the page produces, C consumes
 *
 * Both rings are single-producer single-consumer: head and tail are free-running
 * u32 slot counters (slot = counter % slots, slots a power of two); a ring is
 * empty when head == tail and full when head - tail == slots.  The producer
 * fills a slot, then stores head; the consumer reads the slot, then stores
 * tail.  Every head, tail and bell word is accessed with atomics (Atomics.* on
 * the page, __atomic_* in C).
 *
 * Wake-ups: C bumps tx_bell and notifies it after it queued datagrams (the page
 * waits with Atomics.waitAsync); the page bumps rx_bell and notifies it after it
 * queued received datagrams (the C network thread waits with
 * memory.atomic.wait32, with a timeout so the session timers run).
 *
 * Addresses are IPv4 numbers in host order (a.b.c.d = a<<24 | ...), ports are
 * plain numbers; in memory everything is little-endian (wasm).  On the switch
 * the page sends big-endian frames (web/server-rs/crates/lan):
 *   [0] kind  [1..3] 0  [4..7] source vip  [8..11] destination vip
 *   [12..13] source port  [14..15] destination port  [16..] the payload
 *
 * Two mailboxes use this layout, each registered under its own kind:
 *   HOST_KIND_NET  free roam (runtime/web/roam_web.c): NET_KIND_UDP and
 *                  NET_KIND_UDP_LOOP frames, the payload is the datagram;
 *   HOST_KIND_LAN  LAN play, the Winsock HLE's virtual LAN
 *                  (runtime/web/net_lan.c): NET_KIND_LAN_* frames, the payload
 *                  starts with the 8-byte LAN header below.
 * One WebSocket carries both; the page routes received frames by kind, so the
 * two never see each other's traffic. */
#ifndef NFSU2_NET_PROTO_H
#define NFSU2_NET_PROTO_H
#include <stdint.h>
#include <stddef.h>

#define NET_MAGIC        0x314E544Eu        /* "NTN1" little-endian */
#define NET_VERSION      4u                 /* 2: peer_bits, the LAN mailbox; 3: want, peer_bits for a /22;
                                               4: peer_bits for a /21 */
#define NET_HDR_BYTES    512u
#define NET_SLOT_BYTES   1536u
#define NET_SLOT_HDR     32u
#define NET_MAX_PAYLOAD  1472u              /* an Ethernet MTU's UDP payload */
#define NET_TX_SLOTS     128u
#define NET_RX_SLOTS     256u
#define NET_MAILBOX_BYTES (NET_HDR_BYTES + (NET_TX_SLOTS + NET_RX_SLOTS) * NET_SLOT_BYTES)

/* frame kinds on the switch (byte 0 of a binary frame) */
#define NET_KIND_UDP       1u   /* a UDP datagram between virtual addresses */
#define NET_KIND_UDP_LOOP  2u   /* test harness: a datagram on the emulated loopback
                                   (always sent to the switch's broadcast address;
                                   only test-mode instances take it) */
#define NET_KIND_LAN_UDP   3u   /* LAN play: a UDP datagram (Winsock SOCK_DGRAM) */
#define NET_KIND_LAN_TCP   4u   /* LAN play: a TCP segment of the virtual LAN's own stream protocol */
#define NET_KIND_LAN_ICMP  5u   /* LAN play: an ICMP message (raw socket; echo answered by the receiver) */
#define NET_KIND_NOTICE   16u   /* TX ring only, never on the switch: a line of UTF-8 text the page shows
                                   over the game (free roam: a friend joined, left, went into a race);
                                   the friend's name first, ended by 0x1F (none: a line of the game's own) */
#define NET_KIND_LOBBY    17u   /* TX ring only, never on the switch: the free-roam race lobby's panel over
                                   the game, its state as a JSON object ("{}": none), sent when it changes
                                   (roam_game.c mp_panel) */
#define NET_KIND_SAY      18u   /* TX ring only, never on the switch: quick chat, a phrase said (by a friend near,
                                   one in our race lobby, or the player), as a JSON object (roam_game.c say_*) */
#define NET_KIND_FRIEND   19u   /* TX ring only, never on the switch: the player's car and a friend's touched front
                                   bumpers for 3 s, or parted after that, as a JSON object {"id","name","on"}
                                   (roam_game.c bump_*): the page's "Add friend" */
#define NET_BROADCAST      0xFFFFFFFFu
#define NET_ROAM_HUB       0x0A4D00FDu  /* 10.77.0.253: the server's free-roam hub (web/server-rs/crates/roam),
                                   an address the switch never hands out */

/* LAN header: the first NET_LAN_HDR bytes of a NET_KIND_LAN_* payload
 *   [0] op  [1] ttl (informative)  [2..3] 0  [4..7] conn (TCP: the
 *   connector's random connection id, little-endian; else 0)
 * then the data (at most NET_MAX_PAYLOAD - NET_LAN_HDR bytes). */
#define NET_LAN_HDR        8u
#define NET_LAN_UDP_DATA     0u /* UDP: a datagram */
#define NET_LAN_UDP_UNREACH  1u /* UDP: "port unreachable" back to the sender (no socket on that port) */
#define NET_LAN_TCP_SYN      1u /* TCP: connect */
#define NET_LAN_TCP_SYNACK   2u /* TCP: accepted (queued on a listener) */
#define NET_LAN_TCP_RST      3u /* TCP: refused (no listener, backlog full) or reset (abortive close) */
#define NET_LAN_TCP_DATA     4u /* TCP: stream bytes, in order */
#define NET_LAN_TCP_FIN      5u /* TCP: graceful close of the sender's direction */
#define NET_LAN_TCP_GONE     6u /* TCP, from the page only: source vip left the switch: reset its connections */

/* mode (C): what the endpoint takes */
#define NET_MODE_CLOSED  0u     /* no endpoint: the page drops everything */
#define NET_MODE_VLAN    1u     /* a player: NET_KIND_UDP to our vip or broadcast, on our port */
#define NET_MODE_LOOP    2u     /* test harness: NET_KIND_UDP_LOOP on our port only */
#define NET_MODE_LAN     3u     /* the LAN mailbox: NET_KIND_LAN_* frames, any port */

/* page_state (page) */
#define NET_PAGE_NONE        0u /* nothing attached yet */
#define NET_PAGE_CONNECTING  1u
#define NET_PAGE_ONLINE      2u /* vip and netmask valid */
#define NET_PAGE_OFFLINE     3u /* lost the switch, reconnecting */
#define NET_PAGE_IDLE        4u /* no mailbox wants the switch (want 0 for a while): the page let it go, so
                                   the place is free for another player; it connects again on want */

/* peer_bits: one bit per host of the switch's network (at most a /21) */
#define NET_PEER_BITS     2048u
#define NET_UDP_PORTS_SET 0x80000000u   /* udp_ports: this module says which ports have a socket */

typedef struct {
    /* fixed, written by C before it registers the mailbox */
    uint32_t magic, version, hdr_bytes, slot_bytes;            /*   0 */
    uint32_t tx_slots, rx_slots, tx_off, rx_off;               /*  16: offsets from the mailbox */
    /* C -> page */
    uint32_t mode, port, tx_head, tx_bell;                     /*  32 */
    uint32_t rx_tail, c_alive, want, udp_ports;                /*  48: c_alive: +1 per network-thread loop;
                                                                  want: this endpoint needs the switch now
                                                                  (free roam: while it runs; LAN play: from
                                                                  the game's WSAStartup to its last
                                                                  WSACleanup), C rings tx_bell on a change;
                                                                  udp_ports (LAN play): NET_UDP_PORTS_SET and
                                                                  bit port % 31 of each port a UDP socket is
                                                                  bound to, so the switch sends a LAN broadcast
                                                                  only where a socket takes it (0: not said, an
                                                                  older module: every broadcast), C rings
                                                                  tx_bell on a change */
    /* page -> C */
    uint32_t page_state, vip, netmask, generation;             /*  64: generation: +1 per switch connection */
    uint32_t tx_tail, rx_head, rx_bell, rx_full_drop;          /*  80 */
    uint32_t tx_offline_drop, rx_oversize, switch_peers, solid;       /*  96: solid: free roam's solid cars as the
                                                                  page's menu switched them (0 not switched,
                                                                  1 off, 2 on; roam_plat_solid) */
    double   switch_rtt_ms;                                    /* 112: last ping to the switch */
    double   tx_wait_ms_sum, tx_wait_ms_max;                   /* 120: C queued -> page sent */
    uint32_t tx_wait_n, say;                                   /* 136: say: quick chat, the phrase the player
                                                                  picked (bits 0-7) and a count (bits 8-31) that
                                                                  changes with each pick (roam_plat_say_pick) */
    uint32_t peer_bits[NET_PEER_BITS / 32];                    /* 144: page: bit h set = the host h of our
                                                                  network (vip & ~netmask) is on the switch (us too) */
    uint32_t crews_gen, crews[4], crews_rgb[4];                /* 400: page: free roam's crews as the page's menu
                                                                  changed them, rc_room_hash of each in the
                                                                  player's order (0: none), and their colours
                                                                  (0x00RRGGBB, 0: the default for the place);
                                                                  crews_gen +1 a change (0: not changed), written
                                                                  after the crews and colours (roam_plat_crews) */
    uint32_t room_gen, room_name[16];                          /* 436: page: free roam's room as the page changed
                                                                  it (an invite link), its name in UTF-8 ending in
                                                                  a 0 byte (none: no room), the words little-endian;
                                                                  room_gen +1 a change (0: not changed), written
                                                                  after the name (roam_plat_room) */
    uint32_t pad[2];                                           /* 504 .. 511 */
} net_mailbox;

typedef struct {
    uint32_t len, kind, src_ip, dst_ip;                        /*  0 */
    uint16_t src_port, dst_port; uint32_t pad;                 /* 16 */
    double   t_ms;                                             /* 24: queued at (performance.timeOrigin + now(), ms) */
    uint8_t  data[NET_SLOT_BYTES - NET_SLOT_HDR];              /* 32 */
} net_slot;

_Static_assert(sizeof(net_mailbox) == NET_HDR_BYTES, "net_mailbox is 512 bytes");
_Static_assert(sizeof(net_slot) == NET_SLOT_BYTES, "net_slot is NET_SLOT_BYTES");
_Static_assert(offsetof(net_mailbox, mode) == 32, "mode");
_Static_assert(offsetof(net_mailbox, rx_tail) == 48, "rx_tail");
_Static_assert(offsetof(net_mailbox, want) == 56, "want");
_Static_assert(offsetof(net_mailbox, page_state) == 64, "page_state");
_Static_assert(offsetof(net_mailbox, tx_tail) == 80, "tx_tail");
_Static_assert(offsetof(net_mailbox, tx_offline_drop) == 96, "tx_offline_drop");
_Static_assert(offsetof(net_mailbox, switch_rtt_ms) == 112, "switch_rtt_ms");
_Static_assert(offsetof(net_mailbox, tx_wait_ms_sum) == 120, "tx_wait_ms_sum");
_Static_assert(offsetof(net_mailbox, tx_wait_n) == 136, "tx_wait_n");
_Static_assert(offsetof(net_mailbox, peer_bits) == 144, "peer_bits");
_Static_assert(offsetof(net_mailbox, crews_gen) == 400, "crews_gen");
_Static_assert(offsetof(net_mailbox, crews_rgb) == 420, "crews_rgb");
_Static_assert(offsetof(net_mailbox, room_gen) == 436, "room_gen");
_Static_assert(offsetof(net_mailbox, room_name) == 440, "room_name");
_Static_assert(offsetof(net_slot, src_port) == 16, "src_port");
_Static_assert(offsetof(net_slot, t_ms) == 24, "t_ms");
_Static_assert(offsetof(net_slot, data) == NET_SLOT_HDR, "data");
_Static_assert(NET_MAX_PAYLOAD <= NET_SLOT_BYTES - NET_SLOT_HDR, "payload fits a slot");
#endif
