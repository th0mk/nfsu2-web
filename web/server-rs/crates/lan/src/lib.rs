//! The virtual LAN switch: every browser that joins gets a virtual IPv4 address, and the game's network traffic goes
//! from browser to browser through it, on every core.  It keeps the frames, messages, limits, counts and file of the
//! Node server it replaced (which did all of it on its one thread), rule for rule.
//!
//! # The protocol
//!
//! Every browser that joins gets an address on 10.77.0.0/19 (`MASK`): 250 in each of 10.77.0-23.x (`NETS`, host
//! bytes 2-251, so none ends in .0 or .255 and 10.77.0.1 and .253 stay the server's own), `ADDRS` (6000) in all,
//! handed out in turn across the twenty-four (`vip_at`: 10.77.0.2, 10.77.1.2, ..., 10.77.23.2, 10.77.0.3, ...), so two
//! players are rarely in the same /24 and anything that still takes the network for a /24 (or for the /20 it was
//! before) shows at once, not only past 250 (or 4000) players.  Binary frames carry the game's network traffic; the
//! switch only routes them and never looks inside:
//!
//! ```text
//! [0]     u8  kind (opaque to the switch: datagram, stream open/data/close, ...)
//! [1..3]  reserved
//! [4..7]  u32 source vip      (rewritten here to the sender's own address)
//! [8..11] u32 destination vip (0xFFFFFFFF = BROADCAST, to everyone else)
//! [12..]  payload (ports and data, defined by the runtime's socket layer)
//! ```
//!
//! Numbers are big-endian.  Kinds in use (runtime/web/net_proto.h, web/src/net):
//!
//! - 1 (`KIND_UDP`): a UDP datagram: [12..13] source port, [14..15] destination port, [16..] data.
//! - 2: a UDP datagram on the test harness's emulated loopback (the same layout, always to the broadcast address;
//!   only test-mode instances take it).
//! - 3, 4, 5 (`KIND_LAN_UDP`, `KIND_STREAM`, `KIND_LAN_ICMP`): LAN play (the Winsock HLE, runtime/web/net_lan.c):
//!   UDP, TCP segments and ICMP with ports at [12..15] and an 8-byte LAN header (op, ttl, connection id) before the
//!   data.  Their broadcasts go only to the browsers that have LAN play open (the game's LAN or Online screens:
//!   "lan":1 in the hello, then {"t":"lan","on":0|1}; a page of before says nothing and gets them all): the others
//!   drop them anyway, and on a full switch each one would reach 6000 browsers.
//! - 32 (`KIND_BUNDLE`): a bundle: several frames in one message, [1..3] reserved, then entries of [0] u8 n,
//!   [1] reserved, [2..3] u16 frame length, n x u32 destination vip, the frame: to each of the n (the frame's own
//!   destination is ignored), or with n = 0 to its own.  The switch takes them from anyone (the welcome says
//!   "bundle":1) and sends a client its datagrams as bundles (n = 0) when its hello said "bundle":1: what is queued
//!   for it by the bundle clock's next tick goes out as one message (one frame alone goes as it is; the clock is in
//!   how the work is shared out, below).  Free roam sends each friend the same snapshot 20 times a second: 32 friends
//!   were 640 messages a second each way per player, and each message, not its bytes, is what the server and the
//!   tunnel in front pay for.  LAN play's TCP segments (kind 4) are never bundled by the switch (they have a queue of
//!   their own: `stream_queue`).
//! - 33 (`KIND_COMPACT`): a compact bundle, from the switch to a client whose hello said "compact":1 too: its
//!   datagrams without what the page puts back exactly (the receiver's own address, free roam's port, a free-roam
//!   datagram's magic, version, length and CRC), about a quarter fewer bytes for the same frames (compact.rs).
//!   Never routed from a client (it could name any source), as a bundle in a bundle never is.
//!
//! Text frames are JSON control messages: {"t":"hello","name":...,"deltas":1,"lan":0|1,"bundle":1,"compact":1}
//! (and "id", "claim", "block": below),
//! {"t":"lan","on":0|1} and {"t":"ping","id":n} from a client; {"t":"welcome","vip":...,"vipStr":...,"mask":...,
//! "token":...,"bundle":1}, {"t":"peers",...}, {"t":"delta",...} and {"t":"pong","id":n} from the switch.
//!
//! ## The peer list
//!
//! A connection's first hello names its player (later ones change nothing: the page sends one a connection) and gets
//! it the whole list, {"t":"peers","peers":[{vip,name},...]}.  After that a client whose hello said "deltas":1 (the
//! page) hears only what changed, at most every `peers_ms`: {"t":"delta","peers":[{vip,name} that joined or got their
//! name],"gone":[vip that left (and may be back: the client takes gone first)]}.  One that did not (a page of before),
//! or sent no hello, gets the whole list again, at most every `full_peers_ms`.  (The security review: each rename, 20
//! a second per connection, sent the whole list to everyone: 11,700 times what came in, at 100 players.)
//!
//! A page whose LAN play is shut ("lan":0: free roam alone) gets a list of the server's own hosts only, and no changes,
//! until it opens LAN play ({"t":"lan","on":1}: the whole list then, and the changes after it; shut again, no more
//! changes).  Only LAN play needs the other players' addresses (members.rs `list_for`), and on a full switch the whole
//! list is about 210 KB and every change went to 6000 pages, four times a second.
//!
//! ## Addresses and tokens
//!
//! Addresses belong to the connection that holds them.  The welcome carries a random token (128 bits) that only that
//! client sees.  A client that lost its connection opens /lan?resume (`AttachOpts::resume`) and sends, as its first
//! message, a hello with its old address and that token ({"t":"hello","name":...,"vip":"10.77.x.x","token":...});
//! it gets the address back.  Nobody else can: when a holder leaves, its address stays reserved for its token for
//! `reserve_ms` (not when the page closed the connection itself, 1000 "idle" or "closed": no network feature in use,
//! or the game ended; its address is free at once), and an address is never handed out on request without the token
//! (the audit's F1: /lan?vip= gave a dropped player's address, and the frames other players sent to it, to whoever
//! asked first).  A reservation is never given to another client either, not even when every address is held or
//! reserved: a newcomer then gets "LAN full" until one frees (the review of F1: handing out the oldest reservation let
//! a client that opened and closed connections fast enough take a dropped player's address within 60 ms).  So that
//! one client cannot reserve them all, a client (group) keeps at most `group_reserve` reservations and a site
//! `site_reserve`; past that its own oldest one goes.  A resume with the token also replaces a connection of the same
//! client that the switch still holds (one that died without a close).  Without ?resume the welcome comes at once,
//! as before.
//!
//! With a store (the server's state folder: lan-addresses.json, store.rs), the addresses held and reserved (by a hash
//! of their tokens) outlive a restart of the server: each is reserved for its token again for `reserve_ms` after the
//! start, so the players come back to the same addresses, as they did before tokens.  One that nobody came back for by
//! then counts as a browser that left for the hosts (`Host::peer_gone`): they never saw it leave.
//!
//! ## What a peer learns
//!
//! What a peer learns about the others: their virtual address and the display name they chose (`clean_name`: control
//! and format characters removed, at most 32 characters), nothing else.  The switch is never told who a peer really
//! is (no address, no headers): the server counts connections per client address itself (its /lan handler) and hands
//! the switch only opaque ids for the limits shared by one client's connections (`AttachOpts`): a group (one client
//! address) and a site (an IPv6 /48, which holds 256 client addresses).  Close reasons are fixed texts.
//!
//! ## Limits
//!
//! The limits are `LanOptions` (each field says what it bounds and why), sized to the game: free roam sends 20
//! snapshots a second of about 95 bytes to each friend, 1.9 KB/s per friend, and with 32 friends near about 700 frames
//! and 70 KB a second in all, at the 60 a second Settings allow about 2000 frames and 190 KB; the host of a 3-player
//! LAN race sent at most 100 frames and 7.7 KB a second (the audit's measurement on the switch); LAN play's frames are
//! at most an Ethernet MTU (runtime/web/net_proto.h NET_SLOT_BYTES 1536).
//!
//! A player whose path takes less than that gets fewer of free roam's snapshots, not late ones: the switch times
//! each player's path with a WebSocket ping every `probe_ms`, and while the round trips say its data waits in queues
//! past the switch (the kernel's, cloudflared's, the tunnel's), it passes on only a share of the snapshots sent to
//! it (lag.rs).
//!
//! ## The server's own hosts
//!
//! The Online host (`online`, nfsu2-online): the server's own host on the switch, 10.77.0.1 (an address the switch
//! never hands out).  It is on every peer list as {vip, name, server: true} (so a page lets the game connect to it;
//! the page shows it to nobody), frames addressed to it go to its `Host::frame` instead of to a browser (broadcasts
//! never do; the `FrameCtx` has the sender's opaque group and site ids, for the host's own per-client limits), it
//! answers through [`LanSwitch::send_to`], and its `Host::peer_gone` hears of every browser that left.  What the host
//! sends while it handles a frame (its answer, and what it tells other players because of it: a lobby's chat, a
//! game's players) is charged to the sender's budgets, as a broadcast is (`Cause`); a sender that overspends them has
//! the stream that frame came on closed (the host asks `Charge::overspent`) and pays the debt (at most one burst) off
//! before its next frame gets through.  The host's frames are never dropped on the way (its streams have no
//! retransmission): a receiver with more than `online_queue` queued is not sent more, and the host closes the stream
//! it was for, instead of the whole connection being cut (the Online audit's ONL-2).
//!
//! The free-roam hub (`roam`, nfsu2-roam): 10.77.0.253, the same way: frames to it go to its `Host::frame`, it sends
//! through `send_to` (its own lists, every second: charged to nobody), and its `Host::peer_gone` hears of every
//! browser that left.  It is on no peer list (`Host::listed`).
//!
//! ## Kicks and bans
//!
//! The operator removes a player (the server's control socket, nfsu2-bans): [`LanSwitch::expel`] sends its page a
//! control message, {"t":"kick","msg":...} or {"t":"ban","msg":...,"until":ms} (no until: for good), then closes
//! the connection with [`CLOSE_KICKED`] or [`CLOSE_BANNED`]; the page shows the message and stays off the switch for
//! the rest of that game.  Its address is free at once (no reservation: its token is of no use to it now).  A banned
//! client is turned away the same way before it gets a place: by its address when it connects
//! (`AttachOpts::refuse`, the server checks), or by the id its page keeps in its storage, which the hello names
//! ("id", at most 64 characters: `AttachOpts::on_hello` asks the server).  The switch keeps who was on it for the
//! operator's `who` ([`LanSwitch::who`]): each player's address and name, the opaque id the server gave its client
//! address (`AttachOpts::ident`, a keyed hash) and its page's id, and the same of the players who left in the last
//! `GONE_MS`, so that one who just left can still be banned.  None of it is ever sent to a peer.
//!
//! ## Claimed names
//!
//! A hello may name the token of the keyphrase its page plays with ("claim", at most 64 characters): the server's
//! `AttachOpts::on_claim` makes of it the owner the hosts are told of with each frame (`FrameCtx::owner`, a hash:
//! nfsu2-claims), so the Online lobby and the free-roam hub let a claimed name be played by its keyphrase only.  The
//! token itself is neither kept nor shown, nor ever sent to a peer.
//!
//! ## Blocks
//!
//! A hello may name the players its page blocks in free roam ("block": names as the game shows them over cars, at
//! most 32, each cut to 24 printable characters: members.rs `block_names`): the switch hands them to the free-roam hub
//! with the page's address (`Host::blocks`), which links the two with none of them.  They go with the connection.
//!
//! ## Test aids
//!
//! The trace (`Tap`, the server's --online-trace, nfsu2-online's trace.rs) sees every frame the switch takes from a
//! browser, as sent to its destination; the Online host shows it what it sends itself.  The impairment (the server's
//! --lan-impair) delays, loses and reorders LAN play's frames between browsers (impair.rs).
//!
//! # How the work is shared out
//!
//! Each connection has two tasks on the multi-threaded runtime: a reader and a writer.
//!
//! - The reader reads the client's messages and routes each frame itself, on whatever core it runs: the sender's
//!   own buckets (behind its own lock, uncontended but for the Online host's charges), its client's and site's
//!   (behind theirs: shared only by that client's connections), then the receivers.  No lock is shared by all.
//! - Who is on the switch changes under one lock (`members`: admit, leave, reservations, the peer-list changes,
//!   the store); routing never takes it.  A frame finds its receiver in a fixed table of the 6000 addresses (an
//!   `ArcSwapOption<Peer>` per address: one atomic load), a broadcast walks a copy-on-write list of every peer (LAN
//!   play's, of every peer with LAN play open).  What a broadcast costs its sender is its list's length, known
//!   without a look at the peers, and they are walked only for a broadcast paid for (the second security audit,
//!   2026-10-22: counting the peers each one reached before the budgets made one message of 255 broadcasts walk the
//!   4000 peers 255 times, paid or not, and a few clients kept every core busy that way).
//! - Each receiver has its own queue (`OutQueue`: a short lock, a byte count the senders read for the queue
//!   rules, a wake-up for its writer).  Its writer takes everything queued at once and puts it into one write:
//!   under load many frames leave in one system call.  For a page that takes bundles the writer makes them from the
//!   datagrams queued (one frame alone goes as it is), and free roam's datagrams wait for the next tick of the
//!   bundle clock when nothing else is queued: Node bundled what came in one turn of its event loop, a turn that
//!   grew with the load; here the clock does it (without it every datagram wakes its writer: the .NET port measured
//!   2.19 against 0.94 cores).  Text, stream segments and LAN play's datagrams wake it at once: a race sends each
//!   car 5 to 20 times a second, and a tick's wait (up to 25 ms, at random, at each pass through the switch: two
//!   between joiners, whose updates go through the race's creator) made the other cars wobble.
//! - The clock ticks every `bundle_ms` (50 ms): a page gets at most 20 bundles a second, as many as each friend sends
//!   snapshots (roam_rate 20), however few play.  What a crowd costs is its messages, not its bytes: each is a TCP
//!   segment, which the kernel (and the tunnel in front, cloudflared, again) pays for whole.  2000 players in free
//!   roam with 32 friends each, on 6 cores of a Ryzen 5900X: at 4 ms, 374,000 messages a second and 8.5 cores, not
//!   keeping up (the hub cut their links to 22); at 16 ms, 132,000 messages, 4.2 cores, every link, a snapshot 11 ms
//!   (p50) to 25 ms (p99) from player to player.  Free roam draws friends' cars 100 ms behind the soonest each one's
//!   snapshots came in the last 2 s (roam_core.c off_estimate), so a wait of up to 50 ms here is in that.  The tick
//!   may instead follow the crowd, `bundle_ms` while few take bundles and longer as more do, so that the switch sends
//!   about `bundle_budget` bundles a second in all, up to `bundle_max_ms` (by default the same as `bundle_ms`: fixed);
//!   it did that from 4 to 25 ms until 2026-10-24.
//! - The Online host and the free-roam hub are actors on threads of their own (nfsu2-util `Host`); the switch
//!   posts them their frames in order and they send through [`LanSwitch::send_to`] from their thread.
//! - Counters that every frame touches are sharded (`nfsu2_util::Counter`); the per-peer ones are the peer's own.
//!
//! A receiver's `buffered` count is the bytes handed to its writer and not yet taken by the kernel (what the Node
//! server read as its socket's `bufferedAmount`).  The datagrams waiting for the bundle clock are counted apart
//! (`dgram`), so every rule reads what it read in Node: `roam_queue` and `shed_queue` both, `send_buffer`,
//! `online_queue` and the stream pump `buffered` alone.
//!
//! A frame is never written to in place: a datagram is copied once, by its receiver's writer, into the bytes that
//! go out, with its source (the sender's address) and its destination written there.  So a bundle's frame goes to
//! its 32 receivers as 32 references to one copy of it, and no receiver needs a copy of its own.  That one copy is
//! the router's, of the frame alone: the bytes the sender's reader read are a block of 16 to 64 KB (nfsu2-ws), and a
//! frame of a few bytes waiting in a queue kept all of it (the second security audit: a client sending such frames
//! to a connection of its own that never read, a large message between each two, held about a thousand times what
//! the queues' limits count).
//!
//! # The ws library's behaviour kept
//!
//! The Node server spoke WebSocket through the ws library; what a client saw of it stays.
//!
//! A close frame from the client is answered with one (its code) and the connection ends; the 'close' the switch
//! sees has the client's code and reason (1005 without a code), or 1006 when the connection ended without one.
//! `close(code, reason)` sends a close frame and waits up to 30 s for the client's before the connection goes;
//! `terminate()` ends it at once (1006).  A protocol error (nfsu2-ws `ReadError::Protocol`) sends that close
//! frame (its code only, as ws did) and ends the connection.  Nothing the client sends after a close counts.

mod compact;
mod conn;
pub mod fpred;
mod impair;
mod lag;
mod members;
mod queue;
mod reader;
mod route;
mod store;
mod text;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU8, AtomicU32, AtomicU64, Ordering::Relaxed};
use std::sync::{Arc, OnceLock, Weak};
use std::time::Duration;

use arc_swap::{ArcSwap, ArcSwapOption};
use bytes::Bytes;
use nfsu2_util::{Bucket, Charge, Counter, Host, SendResult, SwitchSend, Tap};
use parking_lot::Mutex;
use serde_json::{Map, Value};
use tokio::io::{AsyncRead, AsyncWrite};
use tokio_util::sync::CancellationToken;

use conn::{Conn, Out};

pub use text::{clean_name, vip_num};

/// 10.77.0.0/19's mask, which the welcome tells the page
pub const MASK: &str = "255.255.224.0";
/// the /24s of it that addresses are handed out in: 10.77.0-23.x
pub const NETS: usize = 24;
/// addresses the switch hands out: host bytes 2-251 in each of the twenty-four /24s
pub const ADDRS: usize = NETS * 250;
pub const BROADCAST: u32 = 0xFFFF_FFFF;
/// the close codes of a kick and a ban (the crate doc's Kicks and bans): the page stays off the switch after them
pub const CLOSE_KICKED: u16 = 4001;
pub const CLOSE_BANNED: u16 = 4003;
/// the players who left in this long are kept for `who` (ms), this many at most
const GONE_MS: i64 = 60 * 60_000;
const GONE_MAX: usize = 2000;

pub(crate) const BASE: u32 = (10 << 24) | (77 << 16);
/// free roam's datagrams (and LAN play's broadcasts): dropped first (`roam_queue`)
const KIND_UDP: u8 = 1;
/// LAN play's UDP and ICMP (with TCP, kinds 3-5): broadcasts only to the browsers with LAN play open
const KIND_LAN_UDP: u8 = 3;
const KIND_LAN_ICMP: u8 = 5;
/// LAN play's TCP segments: never dropped
const KIND_STREAM: u8 = 4;
/// several frames in one message (the crate doc's protocol)
const KIND_BUNDLE: u8 = 32;
/// a receiver with less than this queued gets stream segments at once
const STREAM_LOW: usize = 64 << 10;
/// bytes in one bundle to a client at most
const BUNDLE_OUT: usize = 32 << 10;
/// how long ws waited for the client's close frame after its own (`closeTimeout`)
const CLOSE_TIMEOUT: Duration = Duration::from_secs(30);

/// The switch's limits (the crate doc's Limits says what they are sized to); `Default` has their values.
#[derive(Clone, Debug)]
pub struct LanOptions {
    /// players on the switch at once (one more: in line, queue.rs, or for a page that cannot wait closed with 1013
    /// "LAN full")
    pub max_peers: usize,
    /// pages waiting in line at most (one more: "LAN full"); each holds a WebSocket and nothing else
    pub max_waiting: usize,
    /// a waiting page hears its place in line again at most this often, when it changed
    pub line_tell_ms: u64,
    /// bytes in one binary frame (larger ones are dropped)
    pub max_frame: usize,
    /// bytes in one bundle from a client (larger ones are dropped); one to a client is at most `BUNDLE_OUT`, then the
    /// next begins
    pub max_bundle: usize,
    /// binary frames a second per connection (a token bucket, its burst 2 s worth): 32 friends at 60 snapshots a
    /// second, and the page's answers to stray connects (web/src/net, 32 KB/s at most)
    pub frame_rate: f64,
    /// binary bytes a second per connection (the same)
    pub byte_rate: f64,
    /// the same for all connections of one client address together (the group id the server passes): room for about
    /// 7 players behind one address with 32 friends each
    pub group_frame_rate: f64,
    /// (the same, in bytes)
    pub group_byte_rate: f64,
    /// and for all of one site (the site id)
    pub site_frame_rate: f64,
    /// (the same, in bytes)
    pub site_byte_rate: f64,
    /// control messages a second per connection
    pub text_rate: f64,
    /// bytes in one control message (larger ones are dropped)
    pub max_text: usize,
    /// WebSocket control frames a second (a token bucket, its burst 2 s worth): pings, which the switch answers
    /// itself, and pongs it did not ask for.  No browser sends either (the WebSocket API has no ping), so a client
    /// over the rate is disconnected.
    pub ctl_rate: f64,
    /// datagrams (kinds 1, 2, 3, 5 and unknown kinds) to a receiver with more than this queued are dropped, as a full
    /// link drops UDP: a flood never gets a slower player disconnected
    pub shed_queue: usize,
    /// the same for kind 1 (free roam's snapshots, and LAN play's broadcasts), at a much shorter queue: about a second
    /// of free roam (a SNAP of 94 bytes, 20 a second from each of up to 32 friends).  A car's position is of no use
    /// once it is that old (the game draws friends' cars 100 ms in the past and throws late ones away), so a receiver
    /// whose link falls behind gets the newest ones as soon as it catches up, not seconds of old ones first.  The
    /// queue is the switch's own: the kernel's buffer to the receiver (or to the tunnel) comes before it.
    pub roam_queue: usize,
    /// LAN play's TCP segments (kind 4) are never dropped (the virtual LAN's streams have no retransmission).  A
    /// receiver may have `stream_queue` of them queued, `stream_share` of those from one client (group).  Past that,
    /// whose fault it is decides who goes: a receiver that has stopped reading (nothing it was sent got out for
    /// `stall_ms` while segments waited) is disconnected; otherwise the senders outrun it and the client with the
    /// most queued there is disconnected, all of its connections (the review of the audit's F2: streams from 9
    /// addresses got a reading victim cut, and a receiver that stopped reading got its sender cut).  A LAN race's
    /// host sends about 11 KB of stream data a race.  Each segment counts with what it costs the switch besides its
    /// bytes (route.rs `SEGMENT_COST`).
    pub stream_queue: usize,
    /// (see `stream_queue`)
    pub stream_share: usize,
    /// (see `stream_queue`)
    pub stall_ms: f64,
    /// bytes queued towards one receiver: a receiver that falls this far behind anyway (it stopped reading) is
    /// disconnected (it reconnects and gets its address back).  Checked before everything the switch sends and on
    /// every keepalive tick.
    pub send_buffer: usize,
    /// a WebSocket ping this often, well under a proxy's idle cut (Cloudflare closes a WebSocket that stays quiet for
    /// about 100 s); no pong to that ping (its payload is random: only a client that reads can answer) by the next
    /// ping = the connection is gone (a pong to any of the switch's pings queued since that ping counts: lag.rs)
    pub keepalive_ms: u64,
    /// a WebSocket ping this often besides, whose round trip says how far behind the player's path is, and so the
    /// share of free roam's snapshots it gets (lag.rs; 0: none)
    pub probe_ms: u64,
    /// no message at all from the client for this long: closed (the page pings the switch every 2 s while it is
    /// connected)
    pub idle_ms: i64,
    /// how long a departed player's address waits for its token
    pub reserve_ms: i64,
    /// departed players' addresses kept for one client (group) at most
    pub group_reserve: usize,
    /// and for one site
    pub site_reserve: usize,
    /// how long a ?resume connection may take to send its hello
    pub resume_wait_ms: u64,
    /// bytes queued towards one receiver past which the Online host sends it nothing more (the host closes that
    /// stream instead)
    pub online_queue: usize,
    /// what changed on the peer list goes out at most this often, as one message to each player (a join and its name
    /// in one)
    pub peers_ms: u64,
    /// the whole list to a client that does not take changes, at most this often
    pub full_peers_ms: f64,
    /// the bundle clock's shortest tick (0: no clock, a datagram wakes its writer at once)
    pub bundle_ms: u64,
    /// its longest: the tick grows from `bundle_ms` as more players take bundles, towards `bundle_budget` bundles a
    /// second in all (the crate doc says why); `bundle_ms` or less: a fixed tick
    pub bundle_max_ms: u64,
    pub bundle_budget: u64,
    /// each tick is the tick give or take this share of it, at random (conn.rs `Clock` says why)
    pub bundle_jitter: f64,
    /// friends' car states by their fields to a page whose hello asks for them (fpred.rs); false: as byte deltas still
    pub fpred: bool,
    /// SO_SNDBUF for /lan connections (NFSU2_LAN_SNDBUF; 0: the kernel's own, which Linux grows to megabytes)
    pub send_buffer_os: usize,
    /// the largest WebSocket message a client may send (the server's `limits::WS_MAX_PAYLOAD`, the ws library's
    /// `maxPayload`: a larger one closes the connection with 1009); the switch drops frames over `max_frame` and
    /// bundles over `max_bundle` itself
    pub max_payload: usize,
}

impl Default for LanOptions {
    fn default() -> Self {
        LanOptions {
            max_peers: 128,
            max_waiting: 5000,
            line_tell_ms: 5000,
            max_frame: 8192,
            max_bundle: 64 << 10,
            frame_rate: 2500.0,
            byte_rate: (256 << 10) as f64,
            group_frame_rate: 5000.0,
            group_byte_rate: (512 << 10) as f64,
            site_frame_rate: 10000.0,
            site_byte_rate: (1 << 20) as f64,
            text_rate: 20.0,
            max_text: 1024,
            ctl_rate: 10.0,
            shed_queue: 256 << 10,
            roam_queue: 64 << 10,
            stream_queue: 2 << 20,
            stream_share: 1 << 20,
            stall_ms: 1000.0,
            send_buffer: 8 << 20,
            keepalive_ms: 25_000,
            probe_ms: 1000,
            idle_ms: 10 * 60_000,
            reserve_ms: 60_000,
            group_reserve: 8,
            site_reserve: 16,
            resume_wait_ms: 5_000,
            online_queue: 1 << 20,
            peers_ms: 250,
            full_peers_ms: 1000.0,
            bundle_ms: 50,
            bundle_max_ms: 50,
            bundle_budget: 125_000,
            bundle_jitter: 0.4,
            fpred: true,
            send_buffer_os: 0,
            max_payload: 64 << 10,
        }
    }
}

impl LanOptions {
    /// only finite numbers above 0 are taken from the options: a 0 (or a negative or non-finite rate) keeps the
    /// default.  (`bundle_ms`, `bundle_max_ms`, `send_buffer_os` and `probe_ms` are left as they are: their 0 means what it
    /// says.)
    fn sane(mut self) -> Self {
        let d = LanOptions::default();
        macro_rules! whole {
            ($($f:ident),*) => { $( if self.$f == 0 { self.$f = d.$f; } )* };
        }
        macro_rules! signed {
            ($($f:ident),*) => { $( if self.$f <= 0 { self.$f = d.$f; } )* };
        }
        macro_rules! real {
            ($($f:ident),*) => { $( if !(self.$f.is_finite() && self.$f > 0.0) { self.$f = d.$f; } )* };
        }
        whole!(
            max_peers,
            max_waiting,
            line_tell_ms,
            max_frame,
            max_bundle,
            max_text,
            shed_queue,
            roam_queue,
            stream_queue,
            stream_share,
            send_buffer,
            keepalive_ms,
            group_reserve,
            site_reserve,
            resume_wait_ms,
            online_queue,
            peers_ms,
            max_payload,
            bundle_budget
        );
        signed!(idle_ms, reserve_ms);
        real!(
            frame_rate,
            byte_rate,
            group_frame_rate,
            group_byte_rate,
            site_frame_rate,
            site_byte_rate,
            text_rate,
            ctl_rate,
            stall_ms,
            full_peers_ms
        );
        self
    }
}

/// `AttachOpts::on_join`: (vip, resumed, players on the switch now)
pub type OnJoin = Box<dyn Fn(u32, bool, usize) + Send + Sync>;
/// `AttachOpts::on_leave`: (vip, name, players on the switch now)
pub type OnLeave = Box<dyn Fn(u32, &str, usize) + Send + Sync>;
/// `AttachOpts::on_hello`: the id the hello names -> the control message that turns the client away, or None
pub type OnHello = Box<dyn Fn(&str) -> Option<Bytes> + Send + Sync>;
/// `AttachOpts::on_claim`: the hello's "claim" (a keyphrase's token) -> the owner the hosts are told of, or None
pub type OnClaim = Box<dyn Fn(&str) -> Option<Arc<str>> + Send + Sync>;

/// What the server tells the switch about a new connection (never its address: the crate doc's What a peer learns).
#[derive(Default)]
pub struct AttachOpts {
    /// the client asked for /lan?resume (its first message says which address, with the token, or its place in line
    /// with its ticket)
    pub resume: bool,
    /// the client asked for /lan?queue: it waits in line when every place is taken (queue.rs), instead of "LAN full"
    pub queue: bool,
    /// an opaque id shared by the connections of one client address (their shared limits); None: one of its own
    pub group: Option<String>,
    /// the same for one site (an IPv6 /48), or none
    pub site: Option<String>,
    /// for the server's console: (vip, resumed, players on the switch now)
    pub on_join: Option<OnJoin>,
    /// (vip, name, players on the switch now)
    pub on_leave: Option<OnLeave>,
    /// an opaque id of the client's address for the operator's bans (a keyed hash: never the address), kept for `who`
    pub ident: Option<String>,
    /// a banned client: this control message, then the close with `CLOSE_BANNED`; it never gets a place
    pub refuse: Option<Bytes>,
    /// the hello's id (its page's own): a message back turns the client away as `refuse` does
    pub on_hello: Option<OnHello>,
    /// the hello's "claim" (the token of the keyphrase its page plays with): the owner its frames name to the hosts
    /// (`FrameCtx::owner`); never kept or shown otherwise
    pub on_claim: Option<OnClaim>,
}

/// What the players on the switch do (`LanSwitch::breakdown`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Breakdown {
    pub players: usize,
    pub online: usize,
    pub lan: usize,
    pub roam: usize,
    pub older: usize,
    pub other: usize,
    pub bundles: usize,
    /// of all of them, more than `BEHIND_MS` behind on their path (lag.rs: the last round trip, or a ping unanswered
    /// for that long)
    pub behind: usize,
    /// getting only a share of free roam's snapshots now (lag.rs)
    pub thinning: usize,
    /// the snapshots left out so far (the switch's `thinned`, since it started)
    pub thinned: u64,
    /// what compact bundles carried to pages so far: car states' bytes and entries, how many of those went by their
    /// fields (fpred.rs), the rest's bytes and entries
    pub snap_bytes: u64,
    pub snap_entries: u64,
    pub fpred_entries: u64,
    pub other_bytes: u64,
    pub other_entries: u64,
    /// not on the switch: the pages waiting in line for a place (those away keeping theirs too), and of those that went
    /// in since the line last began, the longest wait (s)
    pub waiting: usize,
    pub waited: u64,
}

/// a player this far behind on its path counts in `Breakdown::behind` (ms)
pub const BEHIND_MS: f64 = 1000.0;

/// Why the switch closed a connection (info()'s `cuts`): a control-frame flood, a queue over `send_buffer` (when
/// sending; on the keepalive tick), no pong to its ping, idle, a client that flooded a receiver with stream segments,
/// a receiver that stopped reading the stream segments it was sent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Cut {
    Flood,
    Slow,
    Queue,
    Dead,
    Idle,
    Hog,
    Stall,
}

const CUT_NAMES: [&str; 7] = ["flood", "slow", "queue", "dead", "idle", "hog", "stall"];

/// The switch's counts (info()'s `frames`, `bytes`, ... and `cuts`): sharded, every frame adds to some.
#[derive(Default)]
pub(crate) struct Stats {
    frames: Counter,
    bytes: Counter,
    dropped: Counter,
    limited: Counter,
    refused: Counter,
    /// datagrams dropped for a receiver's full queue
    shed: Counter,
    /// free roam's snapshots left out for a receiver behind on its path (lag.rs)
    thinned: Counter,
    bundles_in: Counter,
    bundles_out: Counter,
    /// addresses given back to their token
    resumed: Counter,
    /// addresses freed at once: the page closed its connection itself
    released: Counter,
    /// reservations a client gave up for its newer ones (`group_reserve`, `site_reserve`)
    evicted: Counter,
    /// frames the impairment lost
    lost: Counter,
    /// pages that went into line, came back to their place in it, went in from it, left it (queue.rs)
    queued: Counter,
    rejoined: Counter,
    line_in: Counter,
    line_left: Counter,
    cuts: [Counter; 7],
    /// what compact bundles carried to pages: friends' car states (SNAP entries, those by their fields of them) and
    /// the rest, bytes and entries (their 4-byte bundle headers not counted)
    snap_bytes: Counter,
    snap_entries: Counter,
    fpred_entries: Counter,
    other_bytes: Counter,
    other_entries: Counter,
}

impl Stats {
    /// an entry of a compact bundle that went to a page: `data` the datagram, `e` its entry
    pub(crate) fn entry_out(&self, data: &[u8], e: &[u8]) {
        if data.len() > 16 + 5 && data[1] == compact::ROAM_OK && data[16 + 5] == 3 {
            self.snap_bytes.add(e.len() as u64);
            self.snap_entries.inc();
            if e.first() == Some(&fpred::F_FPRED) {
                self.fpred_entries.inc();
            }
        } else {
            self.other_bytes.add(e.len() as u64);
            self.other_entries.inc();
        }
    }

    fn cut(&self, why: Cut) {
        self.cuts[why as usize].inc();
    }
}

/// A token bucket pair: frames and bytes a second.
#[derive(Clone)]
pub(crate) struct Pair {
    frames: Bucket,
    bytes: Bucket,
}

impl Pair {
    fn new(frame_rate: f64, byte_rate: f64) -> Self {
        Pair { frames: Bucket::new(frame_rate), bytes: Bucket::new(byte_rate) }
    }
    /// refilled to `now`: the frames and bytes there
    fn refill(&mut self, now: f64) -> (f64, f64) {
        self.frames.has_at(now, 0.0);
        self.bytes.has_at(now, 0.0);
        (self.frames.v, self.bytes.v)
    }
    /// what `refill` found there paid for some of it (each bucket's `take_at`, once for several)
    fn spend_paid(&mut self, frames: f64, bytes: f64) {
        self.frames.v -= frames;
        self.bytes.v -= bytes;
    }
    /// a host's frame charged: false once a bucket is in debt (both are spent, also when the first is in debt)
    fn spend(&mut self, now: f64, frames: f64, bytes: f64) -> bool {
        let a = self.frames.spend_at(now, frames);
        let b = self.bytes.spend_at(now, bytes);
        a & b
    }
}

/// The limits shared by the connections of one client address (a group) or of one site.
pub(crate) struct Group {
    id: Arc<str>,
    buckets: Mutex<Pair>,
}

/// A text message's and a control frame's buckets: its reader's own.
struct CtlBuckets {
    texts: Bucket,
    ctl: Bucket,
}

const LAN_NULL: u8 = 0;
const LAN_OFF: u8 = 1;
const LAN_ON: u8 = 2;

/// A player on the switch: one connection holding one address.
pub(crate) struct Peer {
    vip: u32,
    conn: Arc<Conn>,
    group: Arc<Group>,
    site: Option<Arc<Group>>,
    token_hash: [u8; 32],
    /// its first hello's name ("player" until then)
    name: OnceLock<Box<str>>,
    /// its first hello came (the name, and deltas: it takes the changes to the peer list)
    hello: AtomicBool,
    deltas: AtomicBool,
    /// it takes bundles (its hello said so)
    bundle: AtomicBool,
    /// LAN play open: said nothing (a page of before), off, on
    lan: AtomicU8,
    frames_in: AtomicU64,
    frames_out: AtomicU64,
    /// its reader posted a frame to the Online host while it handled the message it read last (route.rs; the reader
    /// then waits while the host's backlog is long: `online_backlog`)
    fed_online: AtomicBool,
    /// frames and bytes: its reader's routing and the Online host's charges
    buckets: Mutex<Pair>,
    ctl: Mutex<CtlBuckets>,
    alive: AtomicBool,
    last_in: AtomicI64,
    /// the switch's pings waiting for their pong, and their round trips (lag.rs)
    probe: Mutex<lag::Probe>,
    /// of free roam's snapshots sent to it, the share it gets (lag.rs: SHARE_FULL while its path keeps up)
    share: AtomicU32,
    /// it follows the whole peer list (its LAN play is not shut: members.rs `list_for`); set under the `members` lock
    listed: AtomicBool,
    /// its client's id for the operator's bans (`AttachOpts::ident`), and when it took its place (ms since 1970)
    ident: Option<Arc<str>>,
    since: i64,
}

impl Peer {
    fn name(&self) -> &str {
        self.name.get().map(|n| &**n).unwrap_or("player")
    }
    fn lan(&self) -> Option<bool> {
        match self.lan.load(Relaxed) {
            LAN_OFF => Some(false),
            LAN_ON => Some(true),
            _ => None,
        }
    }
    fn open(&self) -> bool {
        self.conn.open()
    }
    fn takes_bundles(&self) -> bool {
        self.bundle.load(Relaxed)
    }
    /// as `who` shows it
    fn who(&self, doing: &'static str, left: Option<i64>) -> Who {
        Who {
            vip: self.vip,
            name: self.name().to_string(),
            ident: self.ident.clone(),
            browser: self.conn.browser.get().cloned(),
            since: self.since,
            left,
            doing,
        }
    }
}

/// What the switch keeps of a connection while it is open.
pub(crate) struct AttachCtx {
    resume: bool,
    queue: bool,
    group: Arc<Group>,
    site: Option<Arc<Group>>,
    on_join: Option<OnJoin>,
    on_leave: Option<OnLeave>,
    ident: Option<Arc<str>>,
    on_hello: Option<OnHello>,
    on_claim: Option<OnClaim>,
}

/// The sender's buckets while the Online host handles its frame: what the host sends because of it is spent from
/// them, and once one ran dry the host closes that stream (its `overrun`).
struct Cause {
    peer: Arc<Peer>,
    stats: Arc<Stats>,
    over: AtomicBool,
    counted: AtomicBool,
}

impl Charge for Cause {
    fn spend(&self, frames: u32, bytes: usize) {
        let now = nfsu2_util::mono_ms();
        let (f, b) = (frames as f64, bytes as f64);
        // the same order as the reader's: connection, client, site
        let mut ok = self.peer.buckets.lock().spend(now, f, b);
        ok &= self.peer.group.buckets.lock().spend(now, f, b);
        if let Some(s) = &self.peer.site {
            ok &= s.buckets.lock().spend(now, f, b);
        }
        if !ok {
            self.over.store(true, Relaxed);
        }
    }
    fn overspent(&self) -> bool {
        let over = self.over.load(Relaxed);
        if over && !self.counted.swap(true, Relaxed) {
            self.stats.limited.inc();
        }
        over
    }
}

/// the clients (or sites) with connections on the switch, by id: their limits and how many connections share them
type Groups = HashMap<Arc<str>, (Arc<Group>, usize)>;

/// The switch.
pub struct LanSwitch {
    o: LanOptions,
    me: Weak<LanSwitch>,
    /// --lan-impair (test aid, only with the server's --dev)
    impair: Option<impair::Impair>,
    online: Option<Arc<dyn Host>>,
    roam: Option<Arc<dyn Host>>,
    /// their addresses (a host's never changes: looked up once, not for every frame)
    online_vip: Option<u32>,
    roam_vip: Option<u32>,
    /// the frames posted to the Online host and not handled yet (nfsu2_util::Backlog): its senders wait past 4096
    online_backlog: Arc<nfsu2_util::host::Backlog>,
    tap: Option<Tap>,
    store: Option<Arc<store::Store>>,
    /// the peer at each of the 6000 addresses (`slot()`), for unicast without a lock
    table: Box<[ArcSwapOption<Peer>]>,
    /// every peer, in the order of `Members::peers` (broadcasts, the timers, info)
    all: ArcSwap<Vec<Arc<Peer>>>,
    /// every peer whose LAN play is not off (`Members::lan`): LAN play's broadcasts
    lan_all: ArcSwap<Vec<Arc<Peer>>>,
    members: Mutex<members::Members>,
    /// LAN play's stream segments queued at their receivers (rare traffic: one lock for all of them)
    streams: Mutex<route::Streams>,
    groups: Mutex<Groups>,
    sites: Mutex<Groups>,
    anon: AtomicU64,
    conn_ids: AtomicU64,
    stats: Arc<Stats>,
    /// the bundle clock (none at bundle_ms 0)
    clock: Option<Arc<conn::Clock>>,
    /// the players who left in the last `GONE_MS`, oldest first (`who`)
    gone: Mutex<std::collections::VecDeque<Who>>,
    stopped: CancellationToken,
}

/// The bundle clock's tick for `n` players who take bundles: what sends them `bundle_budget` bundles a second in all
/// (one each a tick), from `bundle_ms` up to `bundle_max_ms`
fn bundle_tick(o: &LanOptions, n: u64) -> Duration {
    let lo = o.bundle_ms * 1000;
    let hi = o.bundle_max_ms.max(o.bundle_ms) * 1000;
    Duration::from_micros((n * 1_000_000 / o.bundle_budget.max(1)).clamp(lo, hi))
}

/// The table's index of an address the switch hands out: its /24 and its host byte, the reverse of `vip_at`.
fn slot(vip: u32) -> Option<usize> {
    let host = vip & 255;
    let net = ((vip >> 8) & 255) as usize;
    if vip & 0xFFFF_0000 != BASE || net >= NETS || !(2..=251).contains(&host) {
        return None;
    }
    Some(net + NETS * (host as usize - 2))
}

impl LanSwitch {
    /// The switch: `impair` the --lan-impair spec ("" for none), the hosts (the Online host and the free-roam hub;
    /// give them `switch_send()` afterwards), the dev trace, and the store (the state folder's lan-addresses.json:
    /// the addresses held and reserved, by the hashes of their tokens, outlive a restart).  Starts the switch's
    /// timers: call it on the tokio runtime.
    pub fn new(
        o: LanOptions,
        impair: &str,
        online: Option<Arc<dyn Host>>,
        roam: Option<Arc<dyn Host>>,
        tap: Option<Tap>,
        store: Option<PathBuf>,
    ) -> Arc<LanSwitch> {
        let o = o.sane();
        let store = store.map(|p| Arc::new(store::Store::new(p)));
        let mut m = members::Members::new();
        // the addresses of before a restart: reserved for their tokens again
        if let Some(st) = &store {
            let until = nfsu2_util::now_ms() + o.reserve_ms;
            for (v, hash) in st.load() {
                m.reserved.insert(v, members::Reservation { hash, until, group: None, site: None });
            }
        }
        let clock = (o.bundle_ms > 0).then(|| Arc::new(conn::Clock::new(Duration::from_millis(o.bundle_ms), o.bundle_jitter)));
        let sw = Arc::new_cyclic(|me| LanSwitch {
            impair: impair::Impair::parse(impair),
            online_vip: online.as_ref().map(|h| h.vip()),
            roam_vip: roam.as_ref().map(|h| h.vip()),
            // (about 20 ms of the host's work; its lobby's frames are small, each a copy of its own)
            online_backlog: nfsu2_util::host::Backlog::new(4096, 1024),
            online,
            roam,
            tap,
            store,
            table: (0..ADDRS).map(|_| ArcSwapOption::empty()).collect(),
            all: ArcSwap::from_pointee(Vec::new()),
            lan_all: ArcSwap::from_pointee(Vec::new()),
            members: Mutex::new(m),
            streams: Mutex::new(route::Streams::default()),
            groups: Mutex::new(HashMap::new()),
            sites: Mutex::new(HashMap::new()),
            anon: AtomicU64::new(0),
            conn_ids: AtomicU64::new(0),
            stats: Arc::new(Stats::default()),
            clock,
            gone: Mutex::new(Default::default()),
            stopped: CancellationToken::new(),
            me: me.clone(),
            o,
        });
        sw.start_timers();
        sw
    }

    fn start_timers(self: &Arc<Self>) {
        // WebSocket pings keep proxies from cutting a quiet connection and find dead ones
        let me = self.me.clone();
        let stopped = self.stopped.clone();
        let period = Duration::from_millis(self.o.keepalive_ms);
        tokio::spawn(async move {
            let mut iv = tokio::time::interval_at(tokio::time::Instant::now() + period, period);
            iv.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
            loop {
                tokio::select! {
                    _ = stopped.cancelled() => return,
                    _ = iv.tick() => {}
                }
                let Some(sw) = me.upgrade() else { return };
                sw.keepalive();
            }
        });
        // the line (queue.rs): places that freed up, the waiting pages told where they are
        let (me, stopped) = (self.me.clone(), self.stopped.clone());
        tokio::spawn(async move {
            let mut iv = tokio::time::interval(Duration::from_secs(1));
            iv.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
            loop {
                tokio::select! {
                    _ = stopped.cancelled() => return,
                    _ = iv.tick() => {}
                }
                let Some(sw) = me.upgrade() else { return };
                sw.line_tick();
            }
        });
        // pings that time each player's path (lag.rs)
        if self.o.probe_ms > 0 {
            let (me, stopped) = (self.me.clone(), self.stopped.clone());
            let period = Duration::from_millis(self.o.probe_ms);
            tokio::spawn(async move {
                let mut iv = tokio::time::interval_at(tokio::time::Instant::now() + period, period);
                iv.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
                loop {
                    tokio::select! {
                        _ = stopped.cancelled() => return,
                        _ = iv.tick() => {}
                    }
                    let Some(sw) = me.upgrade() else { return };
                    sw.probe();
                }
            });
        }
        if let Some(clock) = &self.clock {
            conn::Clock::start(Arc::downgrade(clock));
            if self.o.bundle_max_ms > self.o.bundle_ms {
                // every second: the tick for the players who take bundles now
                let (me, stopped) = (self.me.clone(), self.stopped.clone());
                tokio::spawn(async move {
                    let mut iv = tokio::time::interval(Duration::from_secs(1));
                    loop {
                        tokio::select! {
                            _ = stopped.cancelled() => return,
                            _ = iv.tick() => {}
                        }
                        let Some(sw) = me.upgrade() else { return };
                        let n = sw.all.load().iter().filter(|p| p.takes_bundles() && p.open()).count() as u64;
                        if let Some(c) = &sw.clock {
                            c.set_period(bundle_tick(&sw.o, n));
                        }
                    }
                });
            }
        }
        if let Some(im) = &self.impair {
            im.start(self.me.clone());
        }
    }

    /// A new connection, its WebSocket handshake answered already: `r`/`w` its halves, `pre` the bytes read past the
    /// handshake.  Its tasks run until it closes.
    pub fn attach<R, W>(self: &Arc<Self>, r: R, w: W, pre: Bytes, opts: AttachOpts)
    where
        R: AsyncRead + Unpin + Send + 'static,
        W: AsyncWrite + Unpin + Send + 'static,
    {
        let group = self.join_group(false, opts.group.as_deref());
        let site = opts.site.as_deref().map(|s| self.join_group(true, Some(s)));
        let ctx = Arc::new(AttachCtx {
            resume: opts.resume,
            queue: opts.queue,
            group,
            site,
            on_join: opts.on_join,
            on_leave: opts.on_leave,
            ident: opts.ident.map(Arc::from),
            on_hello: opts.on_hello,
            on_claim: opts.on_claim,
        });
        let conn = Arc::new(Conn::new(self.conn_ids.fetch_add(1, Relaxed)));
        // a banned client hears why and goes (its reader answers the close handshake); without ?resume the welcome
        // comes at once; a resume's hello comes first (the reader waits for it)
        if let Some(m) = opts.refuse {
            self.expel_conn(&conn, m, CLOSE_BANNED);
        } else if !ctx.resume {
            self.admit(&conn, &ctx, None);
        }
        tokio::spawn(conn::run(self.clone(), conn, r, w, pre, ctx));
    }

    /// the limits of one client (or site), shared by its connections (a connection without an id has its own)
    fn join_group(&self, site: bool, id: Option<&str>) -> Arc<Group> {
        let key: Arc<str> = match id {
            Some(id) => id.into(),
            None => format!("#{}", self.anon.fetch_add(1, Relaxed) + 1).into(),
        };
        let (map, fr, br) = if site {
            (&self.sites, self.o.site_frame_rate, self.o.site_byte_rate)
        } else {
            (&self.groups, self.o.group_frame_rate, self.o.group_byte_rate)
        };
        let mut m = map.lock();
        let e = m.entry(key.clone()).or_insert_with(|| (Arc::new(Group { id: key, buckets: Mutex::new(Pair::new(fr, br)) }), 0));
        e.1 += 1;
        e.0.clone()
    }

    /// a connection of that client is gone (with its last one, its limits go)
    fn leave_group(&self, site: bool, g: &Arc<Group>) {
        let mut m = if site { self.sites.lock() } else { self.groups.lock() };
        if let Some(e) = m.get_mut(&g.id).filter(|e| Arc::ptr_eq(&e.0, g)) {
            e.1 = e.1.saturating_sub(1);
            if e.1 == 0 {
                m.remove(&g.id);
            }
        }
    }

    /// a host's frame to one browser (never through the impairment), charged to `cause` when given.  Not sent: `Full`
    /// (the receiver is too far behind) or `Gone` (it is not on the switch), and the host closes the stream it was for.
    pub fn send_to(&self, vip: u32, data: Bytes, cause: Option<&dyn Charge>) -> SendResult {
        let guard = slot(vip).map(|i| self.table[i].load());
        let Some(p) = guard.as_ref().and_then(|g| g.as_ref()).filter(|p| p.open()) else {
            self.stats.dropped.inc();
            return SendResult::Gone;
        };
        if p.conn.out.buffered() > self.o.online_queue {
            self.stats.shed.inc();
            return SendResult::Full;
        }
        if let Some(c) = cause {
            c.spend(1, data.len());
        }
        if !self.send(p, Out::Raw(data)) {
            return SendResult::Full;
        }
        p.frames_out.fetch_add(1, Relaxed);
        SendResult::Sent
    }

    /// `send_to` as the closure the hosts take (`attach_switch`)
    pub fn switch_send(self: &Arc<Self>) -> SwitchSend {
        let me = Arc::downgrade(self);
        Arc::new(move |vip, data, cause| match me.upgrade() {
            Some(s) => s.send_to(vip, data, cause),
            None => SendResult::Gone,
        })
    }

    /// players on the switch now
    pub fn peer_count(&self) -> usize {
        self.all.load().len()
    }

    /// a newcomer would wait in line (or get "LAN full") now: others wait, or every place is taken
    pub fn full(&self) -> bool {
        let m = self.members.lock();
        m.line.before_newcomers() || m.full(self.o.max_peers)
    }

    /// pages waiting in line for a place now
    pub fn waiting(&self) -> usize {
        self.members.lock().line.len()
    }

    /// what the players on the switch do, each counted once, in this order: Online mode (the online host's vips()),
    /// the LAN or Online screens open, free roam (the hub's vips()), a page from before the page said whether LAN
    /// play is open (it holds its place until its tab closes), the rest (leaving: the page lets go 10 s after the
    /// last network feature; or starting: free roam not yet heard by the hub); and apart from that, how many of them
    /// take bundles (a page loaded since the switch has them), how many are behind on their path and get fewer
    /// snapshots (lag.rs)
    pub async fn breakdown(&self) -> Breakdown {
        let online = match &self.online {
            Some(h) => h.vips().await.unwrap_or_default(),
            None => Default::default(),
        };
        let roam = match &self.roam {
            Some(h) => h.vips().await.unwrap_or_default(),
            None => Default::default(),
        };
        let all = self.all.load();
        let now = nfsu2_util::mono_ms();
        let mut n = Breakdown { players: all.len(), ..Breakdown::default() };
        for p in all.iter() {
            let lan = p.lan();
            if online.contains(&p.vip) {
                n.online += 1;
            } else if lan == Some(true) {
                n.lan += 1;
            } else if roam.contains(&p.vip) {
                n.roam += 1;
            } else if lan.is_none() {
                n.older += 1;
            } else {
                n.other += 1;
            }
            if p.takes_bundles() {
                n.bundles += 1;
            }
            if p.probe.lock().lag(now) > BEHIND_MS {
                n.behind += 1;
            }
            if p.share.load(Relaxed) < lag::SHARE_FULL {
                n.thinning += 1;
            }
        }
        n.thinned = self.stats.thinned.get();
        let s = &self.stats;
        (n.snap_bytes, n.snap_entries, n.fpred_entries) = (s.snap_bytes.get(), s.snap_entries.get(), s.fpred_entries.get());
        (n.other_bytes, n.other_entries) = (s.other_bytes.get(), s.other_entries.get());
        (n.waiting, n.waited) = self.line_view();
        n
    }

    /// Who is on the switch, then who left it in the last `GONE_MS` (oldest first), for the operator's `who`, kick and
    /// ban (the crate doc's Kicks and bans).  What each one on it does is counted as `breakdown` counts it.
    pub async fn who(&self) -> Vec<Who> {
        let online = match &self.online {
            Some(h) => h.vips().await.unwrap_or_default(),
            None => Default::default(),
        };
        let roam = match &self.roam {
            Some(h) => h.vips().await.unwrap_or_default(),
            None => Default::default(),
        };
        let mut v: Vec<Who> = self
            .all
            .load()
            .iter()
            .map(|p| {
                let doing = if online.contains(&p.vip) {
                    "Online"
                } else if p.lan() == Some(true) {
                    "LAN screens"
                } else if roam.contains(&p.vip) {
                    "free roam"
                } else {
                    ""
                };
                p.who(doing, None)
            })
            .collect();
        let mut g = self.gone.lock();
        let now = nfsu2_util::now_ms();
        while g.front().is_some_and(|w| w.left.is_some_and(|t| now - t > GONE_MS)) {
            g.pop_front();
        }
        v.extend(g.iter().cloned());
        v
    }

    /// The operator removes the player at `vip` (the crate doc's Kicks and bans): its page gets `msg` (a control
    /// message), then the close with `code` (`CLOSE_KICKED` or `CLOSE_BANNED`).  False: nobody is there.
    pub fn expel(&self, vip: u32, msg: Bytes, code: u16) -> bool {
        let guard = slot(vip).map(|i| self.table[i].load());
        let Some(p) = guard.as_ref().and_then(|g| g.as_ref()).filter(|p| p.open()) else { return false };
        self.expel_conn(&p.conn, msg, code);
        true
    }

    /// a connection removed, with or without a place: the message, then the close; its address is not kept for its
    /// token (`closed`), nor its place in line (`left_line`)
    pub(crate) fn expel_conn(&self, conn: &Arc<Conn>, msg: Bytes, code: u16) {
        conn.expelled.store(true, Relaxed);
        conn.push(Out::Text(msg));
        let reason: &[u8] = if code == CLOSE_KICKED { b"kicked" } else { b"banned" };
        conn.close(conn::close_payload(code, reason));
    }

    /// a hello (the first control message of a connection) whose id the server turns away: the connection goes, and
    /// true.  The id is kept for `who` either way, and the owner its claim names for the hosts.
    pub(crate) fn hello_refused(&self, conn: &Arc<Conn>, ctx: &AttachCtx, b: &Bytes) -> bool {
        if b.len() > self.o.max_text || !conn.open() {
            return false;
        }
        let Some(serde_json::Value::Object(m)) = text::parse_json(b) else { return false };
        if m.get("t").and_then(serde_json::Value::as_str) != Some("hello") {
            return false;
        }
        if let Some(owner) = m
            .get("claim")
            .and_then(serde_json::Value::as_str)
            .filter(|c| c.len() <= 64)
            .and_then(|c| ctx.on_claim.as_ref().and_then(|f| f(c)))
        {
            let _ = conn.owner.set(owner);
        }
        let Some(id) = m.get("id").and_then(serde_json::Value::as_str).filter(|i| !i.is_empty() && i.len() <= 64) else {
            return false;
        };
        let _ = conn.browser.set(id.into());
        match ctx.on_hello.as_ref().and_then(|f| f(id)) {
            Some(msg) => {
                self.expel_conn(conn, msg, CLOSE_BANNED);
                true
            }
            None => false,
        }
    }

    /// the switch's counts and its players, as JSON (for the tests: names and virtual addresses only, never a real
    /// address)
    pub async fn info(&self) -> Value {
        let s = &self.stats;
        let peers: Vec<Value> = self
            .all
            .load()
            .iter()
            .map(|p| {
                let mut e = Map::new();
                e.insert("vip".into(), nfsu2_util::vip_str(p.vip).into());
                e.insert("name".into(), p.name().into());
                e.insert("framesIn".into(), p.frames_in.load(Relaxed).into());
                e.insert("framesOut".into(), p.frames_out.load(Relaxed).into());
                Value::Object(e)
            })
            .collect();
        let (reserved, waiting) = {
            let m = self.members.lock();
            (m.reserved.len(), m.line.len())
        };
        let mut m = Map::new();
        m.insert("peers".into(), Value::Array(peers));
        for (k, c) in [
            ("frames", &s.frames),
            ("bytes", &s.bytes),
            ("dropped", &s.dropped),
            ("limited", &s.limited),
            ("refused", &s.refused),
            ("shed", &s.shed),
            ("thinned", &s.thinned),
            ("bundlesIn", &s.bundles_in),
            ("bundlesOut", &s.bundles_out),
            ("snapBytes", &s.snap_bytes),
            ("snapEntries", &s.snap_entries),
            ("fpredEntries", &s.fpred_entries),
            ("otherBytes", &s.other_bytes),
            ("otherEntries", &s.other_entries),
            ("resumed", &s.resumed),
            ("released", &s.released),
            ("queued", &s.queued),
            ("rejoined", &s.rejoined),
            ("lineIn", &s.line_in),
            ("lineLeft", &s.line_left),
        ] {
            m.insert(k.into(), c.get().into());
        }
        m.insert("reserved".into(), reserved.into());
        m.insert("waiting".into(), waiting.into());
        m.insert("evicted".into(), s.evicted.get().into());
        let mut cuts = Map::new();
        for (i, k) in CUT_NAMES.iter().enumerate() {
            cuts.insert((*k).into(), s.cuts[i].get().into());
        }
        m.insert("cuts".into(), Value::Object(cuts));
        if let Some(im) = &self.impair {
            m.insert("impair".into(), im.spec_json());
            m.insert("lost".into(), s.lost.get().into());
        }
        if let Some(h) = &self.online {
            m.insert("online".into(), h.info().await.unwrap_or(Value::Null));
        }
        if let Some(h) = &self.roam {
            m.insert("roam".into(), h.info().await.unwrap_or(Value::Null));
        }
        Value::Object(m)
    }

    /// the timers end, the store is written, the hosts stop
    pub fn stop(&self) {
        self.stopped.cancel();
        self.flush_now();
        if let Some(h) = &self.online {
            h.stop();
        }
        if let Some(h) = &self.roam {
            h.stop();
        }
    }

    /// WebSocket pings keep proxies from cutting a quiet connection and find dead ones (every `keepalive_ms`; public
    /// for the tests)
    #[doc(hidden)]
    pub fn keepalive(&self) {
        let now = nfsu2_util::now_ms();
        {
            let mut m = self.members.lock();
            if self.expire(&mut m) {
                self.persist(&mut m);
            }
        }
        for p in self.all.load().iter() {
            if !p.open() || !self.room(p, Cut::Queue) {
                continue;
            }
            let alive = p.alive.load(Relaxed);
            if !alive || now - p.last_in.load(Relaxed) > self.o.idle_ms {
                self.cut(p, if alive { Cut::Idle } else { Cut::Dead });
                continue;
            }
            p.alive.store(false, Relaxed);
            let data = {
                let mut pr = p.probe.lock();
                let mono = nfsu2_util::mono_ms();
                pr.tick(mono);
                pr.ping(mono)
            };
            p.conn.push(Out::Ping(data));
        }
    }

    /// a ping to every player, its round trip timed (every `probe_ms`; public for the tests)
    #[doc(hidden)]
    pub fn probe(&self) {
        let now = nfsu2_util::mono_ms();
        for p in self.all.load().iter().filter(|p| p.open()) {
            let data = p.probe.lock().ping(now);
            p.conn.push(Out::Ping(data));
        }
    }

    /// a peer the switch disconnects, counted once by why
    fn cut(&self, p: &Peer, why: Cut) {
        if p.conn.terminate() {
            self.stats.cut(why);
        }
    }

    /// room in p's queue (`send_buffer`); a peer that cannot keep up is disconnected instead of buffered without end
    fn room(&self, p: &Peer, why: Cut) -> bool {
        if p.conn.out.buffered() <= self.o.send_buffer {
            return true;
        }
        self.cut(p, why);
        false
    }

    /// to one peer, when there is room
    fn send(&self, p: &Peer, out: Out) -> bool {
        if !self.room(p, Cut::Slow) {
            self.stats.dropped.inc();
            return false;
        }
        p.conn.push(out);
        true
    }

    // ---- for the tests: what they read of the switch's insides ----

    /// the peers in their order (`Members::peers`)
    #[doc(hidden)]
    pub fn peers_view(&self) -> Vec<PeerView> {
        self.all
            .load()
            .iter()
            .map(|p| PeerView {
                vip: p.vip,
                name: p.name().to_string(),
                hello: p.hello.load(Relaxed),
                deltas: p.deltas.load(Relaxed),
                bundle: p.takes_bundles(),
                lan: p.lan(),
                open: p.open(),
                buffered: p.conn.out.buffered(),
            })
            .collect()
    }

    /// the reservations in their order (`Members::reserved`)
    #[doc(hidden)]
    pub fn reserved_view(&self) -> Vec<ReservedView> {
        self.members
            .lock()
            .reserved
            .iter()
            .map(|(v, r)| ReservedView {
                vip: *v,
                until: r.until,
                group: r.group.as_deref().map(str::to_string),
                site: r.site.as_deref().map(str::to_string),
            })
            .collect()
    }

    /// a reservation set as it is given (`until` included)
    #[doc(hidden)]
    pub fn reserve_for_test(&self, vip: u32, hash: [u8; 32], until: i64, group: Option<&str>, site: Option<&str>) {
        self.members
            .lock()
            .reserved
            .insert(vip, members::Reservation { hash, until, group: group.map(Arc::from), site: site.map(Arc::from) });
    }

    #[doc(hidden)]
    pub fn clear_reserved(&self) {
        self.members.lock().reserved.clear();
    }

    /// every reservation ends now (its `until` a millisecond ago)
    #[doc(hidden)]
    pub fn expire_reserved(&self) {
        let now = nfsu2_util::now_ms();
        for r in self.members.lock().reserved.values_mut() {
            r.until = now - 1;
        }
    }

    /// `Members::allocate`: the next free address that is nobody's reservation, or 0
    #[doc(hidden)]
    pub fn allocate(&self) -> u32 {
        let mut m = self.members.lock();
        self.expire(&mut m);
        m.allocate()
    }

    /// every peer's connection ends at once, without a close frame (each one as `terminate()` ends it)
    #[doc(hidden)]
    pub fn terminate_all(&self) {
        for p in self.all.load().iter() {
            p.conn.terminate();
        }
    }
}

/// A peer as the tests see it.
#[doc(hidden)]
#[derive(Clone, Debug)]
pub struct PeerView {
    pub vip: u32,
    pub name: String,
    pub hello: bool,
    pub deltas: bool,
    pub bundle: bool,
    pub lan: Option<bool>,
    pub open: bool,
    /// bytes queued to it and not yet taken by the kernel (its `OutQueue::buffered`)
    pub buffered: usize,
}

/// A player on the switch, or one who left in the last `GONE_MS`, as the operator's `who` sees it.
#[derive(Clone, Debug)]
pub struct Who {
    pub vip: u32,
    pub name: String,
    /// its client's id for bans (`AttachOpts::ident`) and its page's (the hello's "id"), when it had them
    pub ident: Option<Arc<str>>,
    pub browser: Option<Arc<str>>,
    /// when it took its place, and when it left (ms since 1970; None: on the switch now)
    pub since: i64,
    pub left: Option<i64>,
    /// what it does now: "Online" (in the game's Online mode), "LAN screens" (the LAN or Online screens open),
    /// "free roam", or "" (the menus, starting or leaving; and the ones who left)
    pub doing: &'static str,
}

/// A reservation as the tests see it.
#[doc(hidden)]
#[derive(Clone, Debug)]
pub struct ReservedView {
    pub vip: u32,
    pub until: i64,
    pub group: Option<String>,
    pub site: Option<String>,
}

/// the i-th address in the order they are handed out (round the twenty-four /24s: the crate doc says why)
pub fn vip_at(i: usize) -> u32 {
    BASE | ((i % NETS) << 8) as u32 | ((i / NETS) as u32 + 2)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_bundle_tick_is_50_ms_or_follows_the_crowd() {
        assert_eq!(bundle_tick(&LanOptions::default(), 0), Duration::from_millis(50));
        assert_eq!(bundle_tick(&LanOptions::default(), 5000), Duration::from_millis(50));
        let o = LanOptions { bundle_ms: 4, bundle_max_ms: 25, ..LanOptions::default() };
        assert_eq!(bundle_tick(&o, 0), Duration::from_millis(4));
        assert_eq!(bundle_tick(&o, 500), Duration::from_millis(4));
        assert_eq!(bundle_tick(&o, 1000), Duration::from_millis(8));
        assert_eq!(bundle_tick(&o, 2000), Duration::from_millis(16));
        assert_eq!(bundle_tick(&o, 5000), Duration::from_millis(25));
        let fixed = LanOptions { bundle_ms: 10, bundle_max_ms: 10, ..LanOptions::default() };
        assert_eq!(bundle_tick(&fixed, 2000), Duration::from_millis(10));
    }

    #[test]
    fn slots_are_the_order_addresses_are_handed_out_in() {
        for i in 0..ADDRS {
            assert_eq!(slot(vip_at(i)), Some(i));
        }
        for v in [
            BASE,
            BASE | 1,
            BASE | 252,
            BASE | 255,
            BASE | 0x1800 | 2,
            BASE | 0x1F00 | 2,
            BASE | 0x2000 | 2,
            0,
            BROADCAST,
            BASE | 0x1700 | 0xFD,
        ] {
            assert_eq!(slot(v), None, "{v:x}");
        }
    }

    #[test]
    fn options_keep_the_defaults_for_zero() {
        let o = LanOptions { max_peers: 0, frame_rate: -1.0, idle_ms: 0, stall_ms: f64::NAN, bundle_ms: 0, ..LanOptions::default() }.sane();
        let d = LanOptions::default();
        assert_eq!(o.max_peers, d.max_peers);
        assert_eq!(o.frame_rate, d.frame_rate);
        assert_eq!(o.idle_ms, d.idle_ms);
        assert_eq!(o.stall_ms, d.stall_ms);
        assert_eq!(o.bundle_ms, 0);
    }
}
