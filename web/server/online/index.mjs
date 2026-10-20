// The server's stand-in for EA's online servers of NFS Underground 2, for the
// game's own Online mode (main menu, Online) between browsers on this server.
//
//   browser: lifted DirtySock -> winsock.c (pcnfs05.ea.com = 10.77.0.1)
//            -> runtime/web/net_lan.c -> page -> /lan WebSocket -> lan.mjs switch
//   switch:  frames to 10.77.0.1 -> OnlineHost.frame() -> vhost.mjs (TCP, ICMP)
//            -> the services below, by port
//     20920  redirector: @dir -> our lobby's address (no @tic: no RC4, no SSL)
//     20922  lobby: login (any name but a slur), personas, rooms, chat, games (lobby.mjs)
//     13505  EA Messenger (buddy server): friends, presence, messages, invites (buddy.mjs)
//     80     the news and terms pages the config names (http.mjs)
// Race traffic never comes here: the players' browsers send it to each other
// through the switch (UDP 3658, a star around the game's creator), as in LAN
// play.  Every address a player learns is a virtual one (10.77.x.x).
//
// What the server keeps: sessions, rooms, games and chat in memory only; on
// disk only the races' public results per persona (stats.mjs) and each
// persona's friends list (buddy.mjs), names only, under the server's --state
// directory.  Room passwords are checked against randomly keyed digests in
// memory only.  Account passwords, the CD key and the file checksum are
// passed over undecoded (aries.mjs SECRET_KEYS).  The log names players by
// persona and virtual address only, never their real address or password.
// The lines a client causes (logins, games, races, complaints, friends) go
// through log(line, key) with that client's opaque group id as the key: the
// server shows at most a few a minute per client (server.mjs logLimited; the
// Online audit's ONL-8).
import { VirtualHost, VHOST_VIP, VHOST_NAME } from './vhost.mjs';
import { Lobby } from './lobby.mjs';
import { Stats } from './stats.mjs';
import { httpService } from './http.mjs';
import { Buddies } from './buddy.mjs';

export const PORTS = Object.freeze({ redirector: 20920, lobby: 20922, buddy: 13505, http: 80 });

export class OnlineHost {
  /** opts: { log(line, key) (key: the client's group id for a line a client caused, else none), tap(frame) (the
   *  dev trace), limits (lobby.mjs LOBBY_LIMITS), buddyLimits (buddy.mjs BUDDY_LIMITS), now(), stateDir (where
   *  stats.json and buddies.json are kept; none: in memory only) } */
  constructor(opts = {}) {
    this.log = opts.log ?? (s => console.log(s));
    this.send = () => false;
    this.here = new Set();               // the browsers that spoke to this host and are still on the switch
    this.vh = new VirtualHost({ out: (vip, f) => this.send(vip, f), tap: opts.tap, opts: opts.vhost ?? {} });
    const stats = new Stats({ dir: opts.stateDir ?? null, now: opts.now, log: this.log });
    this.lobby = new Lobby({ log: this.log, limits: opts.limits, now: opts.now, ports: PORTS, stats });
    this.buddies = new Buddies({ lobby: this.lobby, log: this.log, now: opts.now, dir: opts.stateDir ?? null, limits: opts.buddyLimits });
    this.vh.listen(PORTS.redirector, c => this.lobby.redirector(c));
    this.vh.listen(PORTS.lobby, c => this.lobby.connect(c));
    this.vh.listen(PORTS.buddy, c => this.buddies.connect(c));
    this.vh.listen(PORTS.http, httpService);
  }
  get vip() { return VHOST_VIP; }
  get name() { return VHOST_NAME; }
  /** the switch's way to its browsers: send(vip, frame) */
  attachSwitch(send) { this.send = send; }
  /** a frame from a browser (ctx: its client's { group, site } ids, from the switch) */
  frame(from, data, ctx) { this.here.add(from); this.lobby.seen(from); this.vh.frame(from, data, ctx); }
  /** what the host sent because of that frame was more than its client's share: its stream goes */
  overrun(from, data) { this.vh.overrun(from, data); }
  peerGone(vip) { this.here.delete(vip); this.vh.peerGone(vip); this.lobby.peerGone(vip); }
  info() { return { ...this.vh.info(), ...this.lobby.info(v => this.here.has(v)) }; }
  /** the browsers in Online mode now (the ones info() counts) */
  vips() { return this.lobby.vips(v => this.here.has(v)); }
  stop() { this.lobby.stop(); this.buddies.stop(); }
}
