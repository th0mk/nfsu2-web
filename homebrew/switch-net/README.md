# n2n: the network client for a native build (Nintendo Switch homebrew)

A small C client for the server's virtual LAN (`web/server-rs/crates/lan`), the same conversation as
`web/src/net/index.ts` has on the page. It is the network part of a native port only: nothing here runs the game.

What it does: opens `/lan` as a WebSocket, says hello, takes the welcome (the player's virtual address `10.77.x.y` and a
token to get it back), keeps the peer list, pings the switch every 10 s, and sends and receives the binary frames
between virtual addresses (`runtime/web/net_proto.h`: free roam is `N2N_ROAM_HUB`, port `N2N_ROAM_PORT`). It asks for
plain frames, not the compact/delta encodings, and unpacks the switch's bundles (kind 32).

## Use

```c
n2n *c = n2n_new("wss://your.site", "Player", install_id, on_event, user);
n2n_connect(c, 5000);                      // after a drop, again: the same address, or the same place in line
while (running) {
    n2n_poll(c, 16);                       // events: WELCOME, PEERS, PONG, QUEUE, FRAME, REMOVED, CLOSED
    n2n_send_udp(c, friend_vip, 47210, 47210, data, len);
}
```

`install_id` is a stable id kept on the SD card (the operator's bans name it). `n2n_cli.c` is a complete example.

## Test (a fake switch, on any host with `cc` and Node)

    homebrew/switch-net/test/run.sh

`test/fake-switch.mjs` checks what the client sends (masking, the escaped hello, no compact/delta, the frame to the
hub) and sends what the real switch sends (welcome, peers with `\u` escapes, a bundle, a text message in two
fragments with a ping between them, a delta, a kick and the close 4001). It also passes under
`CFLAGS="-fsanitize=address,undefined"`. It has not been run against the real server.

## On the Switch (not built yet)

- devkitPro with libnx; call `socketInitializeDefault()` first. `n2n_sock.c` is plain BSD sockets, as it is.
- `wss://` needs `n2n_tls_mbedtls.c` (`-DN2N_TLS`, `-lmbedtls -lmbedx509 -lmbedcrypto`). It is **not tested**: call
  `n2n_tls_set_ca(pem)` with the root of your site's certificate chain first. It refuses to connect without one.
- The server accepts a client without an `Origin` header, as this one is, but it limits connections per address and
  per site, and sends `{"t":"queue"}` when it is full.

## Not done

- The compact, delta and fpred encodings (`web/src/net/proto.ts`): the switch sends about a third more bytes to this
  client. Add them once a game talks through it.
- Sending bundles (kind 32): free roam sends each friend the same snapshot 20 times a second, and the server pays per
  message, so a real client should bundle them.
- The game side: `runtime/web/roam_web.c` and `net_lan.c` talk to the page through the mailbox in `net_proto.h`;
  a native build replaces that mailbox with calls to this client.
