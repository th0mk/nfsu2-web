//! A connection: its queue (what the switch sends it, taken by its writer all at once), its writer, the bundle clock,
//! and the ws library's states and close rules (the WebSocket library of the Node server: the crate doc's last part).

use std::sync::atomic::{
    AtomicBool, AtomicU8, AtomicU32, AtomicU64, AtomicUsize,
    Ordering::{AcqRel, Acquire, Relaxed, Release, SeqCst},
};
use std::sync::{Arc, Weak};
use std::time::Duration;

use bytes::Bytes;
use nfsu2_ws::frame;
use parking_lot::Mutex;
use tokio::io::{AsyncRead, AsyncWrite, AsyncWriteExt};
use tokio::sync::Notify;
use tokio_util::sync::CancellationToken;

use crate::{AttachCtx, BUNDLE_OUT, CLOSE_TIMEOUT, KIND_BUNDLE, LanSwitch, Peer, Stats, compact};

/// how a page takes its datagrams (`Conn::compact`): as bundles of before, compact bundles, compact bundles with deltas,
/// and with friends' car states by their fields (fpred.rs)
pub(crate) const PLAIN: u8 = 0;
pub(crate) const COMPACT: u8 = 1;
pub(crate) const DELTAS: u8 = 2;
pub(crate) const FPRED: u8 = 3;

/// ws's readyState
const OPEN: u8 = 1;
const CLOSING: u8 = 2;
const CLOSED: u8 = 3;

/// Something for a connection's writer.
pub(crate) enum Out {
    /// a text message (a control message of the switch's)
    Text(Bytes),
    /// a binary message as it is (a host's frame)
    Raw(Bytes),
    /// a frame from a browser, alone: its source and destination written in as it goes out
    Frame {
        data: Bytes,
        src: u32,
        dst: u32,
    },
    /// a datagram to a peer that takes bundles: into the writer's next bundle (one alone goes as a `Frame`); `at`: when
    /// it was queued (`now_us`), for a free-roam ping or pong only (compact.rs `timed`), else 0
    Dgram {
        data: Bytes,
        src: u32,
        dst: u32,
        at: u32,
    },
    Ping([u8; 8]),
    Pong(Bytes),
    /// a close frame's payload; nothing goes after it
    Close(Bytes),
    /// both close frames went: the writer ends its half of the connection (ws `socket.end()`)
    End,
}

/// a frame's header for a payload of `len` (RFC 6455 5.2: 2, 4 or 10 bytes from a server)
fn header_len(len: usize) -> usize {
    if len < 126 {
        2
    } else if len < 65536 {
        4
    } else {
        10
    }
}

impl Out {
    /// the bytes it puts on the wire, as `buffered` counts them (ws's bufferedAmount counts the frame's header too);
    /// a datagram for a bundle counts in `dgram` instead (4 + its length, its entry in the bundle)
    fn wire_len(&self) -> usize {
        match self {
            Out::Text(b) | Out::Raw(b) | Out::Pong(b) | Out::Close(b) => header_len(b.len()) + b.len(),
            Out::Frame { data, .. } => header_len(data.len()) + data.len(),
            Out::Ping(p) => header_len(p.len()) + p.len(),
            Out::Dgram { .. } | Out::End => 0,
        }
    }
}

/// What the switch has queued for one connection.
pub(crate) struct OutQueue {
    pub(crate) q: Mutex<Vec<Out>>,
    /// bytes handed to the writer and not yet taken by the kernel (the ws library's `bufferedAmount`)
    buffered: AtomicUsize,
    /// datagrams waiting for the bundle clock (their bytes as bundle entries)
    dgram: AtomicUsize,
    wake: Notify,
    /// on the bundle clock's list for its next tick
    armed: AtomicBool,
}

impl OutQueue {
    pub(crate) fn buffered(&self) -> usize {
        self.buffered.load(Relaxed)
    }
    pub(crate) fn dgram(&self) -> usize {
        self.dgram.load(Relaxed)
    }
    /// `buffered` for the stream pump.  A sender that leaves segments waiting at the switch sets `streaming` and then
    /// reads `buffered`; the writer lowers `buffered` and then reads `streaming` (and pumps when it is set).  With
    /// both sides SeqCst one of them sees the other's write, so the segments go after the writer's next write; with
    /// Relaxed both could miss (x86 lets a load pass an earlier store) and they waited for whatever woke the writer
    /// next, up to the keepalive's ping.  (On x86 the writer's side costs nothing more: its decrements are locked
    /// instructions either way, its load a plain one.)
    pub(crate) fn buffered_synced(&self) -> usize {
        self.buffered.load(SeqCst)
    }
}

/// The close handshake as ws kept it: whether its close frame went, the client's (code and reason), a protocol error.
#[derive(Default)]
struct CloseState {
    sent: bool,
    received: Option<(u16, Bytes)>,
    errored: bool,
}

/// A connection's own state, before and after it holds an address.
pub(crate) struct Conn {
    pub(crate) id: u64,
    state: AtomicU8,
    /// ends both tasks at once (ws `terminate()`, the close timer, a write that failed)
    pub(crate) kill: CancellationToken,
    killed: AtomicBool,
    pub(crate) out: OutQueue,
    close: Mutex<CloseState>,
    /// the peer it holds an address as, from its admission until its close logic took it
    pub(crate) peer: Mutex<Option<Arc<Peer>>>,
    /// stream segments wait at the switch for this connection (a hint for its writer: pump after each write)
    pub(crate) streaming: AtomicBool,
    /// how its datagrams go (its hello said "compact":1, "delta":1 and "fpred":1: compact.rs): `PLAIN`, `COMPACT`,
    /// `DELTAS` or `FPRED`;
    /// compact ones leave out the address it holds.  One word, so that its writer never sees compact without the
    /// deltas its page takes (the page keeps the last bodies of every compact bundle it was sent)
    pub(crate) compact: AtomicU8,
    pub(crate) vip: AtomicU32,
    /// while it waits in line for a place (queue.rs): its place, and what it sent meanwhile
    pub(crate) wait: Mutex<crate::queue::Wait>,
    /// the operator removed it (a kick or a ban: `LanSwitch::expel_conn`)
    pub(crate) expelled: AtomicBool,
    /// the id its page keeps (its hello's "id"), for `who`
    pub(crate) browser: std::sync::OnceLock<Arc<str>>,
    /// the keyphrase its page plays with, as the server knows it (its hello's "claim": `AttachOpts::on_claim`)
    pub(crate) owner: std::sync::OnceLock<Arc<str>>,
}

impl Conn {
    pub(crate) fn new(id: u64) -> Self {
        Conn {
            id,
            state: AtomicU8::new(OPEN),
            kill: CancellationToken::new(),
            killed: AtomicBool::new(false),
            out: OutQueue {
                q: Mutex::new(Vec::new()),
                buffered: AtomicUsize::new(0),
                dgram: AtomicUsize::new(0),
                wake: Notify::new(),
                armed: AtomicBool::new(false),
            },
            close: Mutex::new(CloseState::default()),
            peer: Mutex::new(None),
            streaming: AtomicBool::new(false),
            compact: AtomicU8::new(PLAIN),
            vip: AtomicU32::new(0),
            wait: Mutex::new(Default::default()),
            expelled: AtomicBool::new(false),
            browser: Default::default(),
            owner: Default::default(),
        }
    }

    /// ws `readyState === OPEN`
    pub(crate) fn open(&self) -> bool {
        self.state.load(Acquire) == OPEN
    }

    /// ws `terminate()`: the connection goes at once (its close logic follows on its own task).  True for the call
    /// that ended it (so that a cut is counted once).
    pub(crate) fn terminate(&self) -> bool {
        if self.killed.swap(true, AcqRel) {
            return false;
        }
        let _ = self.state.compare_exchange(OPEN, CLOSING, AcqRel, Acquire);
        self.kill.cancel();
        true
    }

    /// the connection is over (its reader ended): nothing is sent to it any more
    fn end(&self) {
        self.killed.store(true, Release);
        self.state.store(CLOSED, Release);
        self.kill.cancel();
    }

    /// queued to the writer, which wakes at once (while the connection is open: ws sends nothing after a close)
    pub(crate) fn push(&self, item: Out) {
        if !self.open() {
            return;
        }
        self.push_any(item);
    }

    fn push_any(&self, item: Out) {
        let n = item.wire_len();
        {
            let mut q = self.out.q.lock();
            q.push(item);
            // counted under the lock: the writer subtracts only what it took
            self.out.buffered.fetch_add(n, Relaxed);
        }
        self.out.wake.notify_one();
    }

    /// a datagram for a peer that takes bundles: it waits for the bundle clock (or goes at once without one: no clock,
    /// or LAN play's, route.rs `post`); a
    /// bundle's worth (BUNDLE_OUT) wakes the writer at once: a full bundle does not wait for the tick
    pub(crate) fn push_dgram(self: &Arc<Self>, data: Bytes, src: u32, dst: u32, clock: Option<&Clock>) {
        if !self.open() {
            return;
        }
        let n = 4 + data.len();
        let total = {
            let mut q = self.out.q.lock();
            let at = if compact::timed(&data) { now_us() | 1 } else { 0 };
            q.push(Out::Dgram { data, src, dst, at });
            self.out.dgram.fetch_add(n, Relaxed) + n
        };
        match clock {
            Some(c) if total <= BUNDLE_OUT => {
                if !self.out.armed.load(Relaxed) && !self.out.armed.swap(true, AcqRel) {
                    c.arm(self);
                }
            }
            _ => self.out.wake.notify_one(),
        }
    }

    /// ws `close(code, reason)` with the close frame's payload: the frame, then up to 30 s for the client's
    pub(crate) fn close(self: &Arc<Self>, payload: Bytes) {
        let _cs = self.close.lock();
        if self.state.compare_exchange(OPEN, CLOSING, AcqRel, Acquire).is_ok() {
            self.push_any(Out::Close(payload));
            self.close_timer();
        }
    }

    /// the client's close frame (ws `receiverOnConclude`): answered with its code (none for 1005), and once both
    /// went the connection ends
    pub(crate) fn close_received(self: &Arc<Self>, code: u16, reason: Bytes) {
        let mut cs = self.close.lock();
        let answer = close_payload(code, &reason);
        cs.received = Some((code, reason));
        if self.state.compare_exchange(OPEN, CLOSING, AcqRel, Acquire).is_ok() {
            self.push_any(Out::Close(answer));
            self.close_timer();
        } else if cs.sent {
            self.push_any(Out::End);
        }
    }

    /// a protocol error (ws `receiverOnError`): a close frame with the error's code only
    pub(crate) fn protocol_error(self: &Arc<Self>, code: u16) {
        let mut cs = self.close.lock();
        cs.errored = true;
        if self.state.compare_exchange(OPEN, CLOSING, AcqRel, Acquire).is_ok() {
            self.push_any(Out::Close(Bytes::copy_from_slice(&code.to_be_bytes())));
            self.close_timer();
        } else if cs.sent {
            self.push_any(Out::End);
        }
    }

    /// the writer sent the close frame: true when the connection may end now (the client's came, or it broke the
    /// protocol)
    fn close_sent(&self) -> bool {
        let mut cs = self.close.lock();
        cs.sent = true;
        cs.received.is_some() || cs.errored
    }

    /// what the switch's 'close' sees: the client's close frame, or 1006 without one
    pub(crate) fn close_code(&self) -> (u16, Bytes) {
        self.close.lock().received.clone().unwrap_or((1006, Bytes::new()))
    }

    /// its page let go of its place itself (1000 "idle" or "closed": no network feature in use, or the game ended),
    /// or the operator removed it: nothing is kept for it (its address, its place in line)
    pub(crate) fn let_go(&self) -> bool {
        let (code, reason) = self.close_code();
        (code == 1000 && (&reason[..] == b"idle" || &reason[..] == b"closed")) || self.expelled.load(Relaxed)
    }

    /// ws's closeTimeout: a client that never answers the close frame is cut after 30 s
    fn close_timer(self: &Arc<Self>) {
        let me = self.clone();
        tokio::spawn(async move {
            tokio::select! {
                _ = me.kill.cancelled() => {}
                _ = tokio::time::sleep(CLOSE_TIMEOUT) => { me.terminate(); }
            }
        });
    }
}

/// a close frame's payload for a code and its reason (1005: none, ws's `close()`)
pub(crate) fn close_payload(code: u16, reason: &[u8]) -> Bytes {
    if code == 1005 {
        return Bytes::new();
    }
    let mut p = Vec::with_capacity(2 + reason.len());
    p.extend_from_slice(&code.to_be_bytes());
    p.extend_from_slice(reason);
    Bytes::from(p)
}

/// A connection's tasks: its reader (which routes what it reads) and its writer; when the reader ends (the client
/// went, or the kill token: `terminate()`, the close timer, a failed write), the connection is over: the writer is
/// stopped and the close logic runs once (also when a task panicked: the rest of the switch goes on).
pub(crate) async fn run<R, W>(sw: Arc<LanSwitch>, conn: Arc<Conn>, r: R, w: W, pre: bytes::Bytes, ctx: Arc<AttachCtx>)
where
    R: AsyncRead + Unpin + Send + 'static,
    W: AsyncWrite + Unpin + Send + 'static,
{
    let writer = tokio::spawn(write_loop(sw.clone(), conn.clone(), w));
    let reader = tokio::spawn(crate::reader::read_loop(sw.clone(), conn.clone(), r, pre, ctx.clone()));
    let _ = reader.await;
    conn.end();
    // (the writer waits on nothing but its queue and its socket: stopping it here spares it a wait on the kill token
    // for each wake-up and each write, the switch's hottest path)
    writer.abort();
    let _ = writer.await;
    sw.closed(&conn, &ctx);
}

/// a panic in the writer ends its connection too
struct KillOnPanic(Arc<Conn>);

impl Drop for KillOnPanic {
    fn drop(&mut self) {
        if std::thread::panicking() {
            self.0.terminate();
        }
    }
}

/// The largest write buffer (bytes) and list of items a writer keeps for its next wake-up (two bundles' worth)
const KEEP_BUF: usize = 2 * BUNDLE_OUT;
const KEEP_ITEMS: usize = 1024;

/// The writer: everything queued at once into one write.  It runs until its connection's supervisor stops it.
async fn write_loop<W: AsyncWrite + Unpin>(sw: Arc<LanSwitch>, conn: Arc<Conn>, mut w: W) {
    let _guard = KillOnPanic(conn.clone());
    let mut items: Vec<Out> = Vec::new();
    let mut buf: Vec<u8> = Vec::new();
    let mut close_sent = false;
    // a compact bundle as it is made, and the last bodies its page was sent (compact.rs `Refs`)
    let mut body: Vec<u8> = Vec::new();
    let mut refs = compact::Refs::default();
    let mut fp = crate::fpred::Fpred::default();
    loop {
        conn.out.wake.notified().await;
        loop {
            {
                let mut q = conn.out.q.lock();
                if q.is_empty() {
                    // (the queue's list is the writer's of the turn before: it may be the burst's)
                    if q.capacity() > KEEP_ITEMS {
                        *q = Vec::new();
                    }
                    break;
                }
                std::mem::swap(&mut *q, &mut items);
                conn.out.dgram.store(0, Relaxed);
            }
            let mode = conn.compact.load(Relaxed);
            let c = (mode != PLAIN).then(|| Compact {
                own: conn.vip.load(Relaxed),
                refs: (mode >= DELTAS).then_some(&mut refs),
                fp: (mode == FPRED).then_some(&mut fp),
                body: &mut body,
            });
            let e = encode(&sw.stats, &items, &mut buf, &mut close_sent, c);
            conn.out.buffered.fetch_add(e.extra, Relaxed);
            // what the kernel takes leaves `buffered` as it goes (what was skipped after a close frame at the end)
            let mut at = 0;
            while at < buf.len() {
                match w.write(&buf[at..]).await {
                    Ok(n) if n > 0 => {
                        at += n;
                        conn.out.buffered.fetch_sub(n, SeqCst);
                    }
                    _ => {
                        // the socket failed (ws: destroyed, 'close' with 1006)
                        conn.terminate();
                        return;
                    }
                }
            }
            conn.out.buffered.fetch_sub(e.counted + e.extra - buf.len(), SeqCst);
            // (a TLS stream holds what it took until it is flushed)
            if !buf.is_empty() && w.flush().await.is_err() {
                conn.terminate();
                return;
            }
            buf.clear();
            items.clear();
            if e.end || (e.close && conn.close_sent()) {
                let _ = w.shutdown().await;
                return;
            }
            // (SeqCst: see OutQueue::buffered_synced)
            if conn.streaming.load(SeqCst) {
                sw.pump(&conn);
            }
        }
        // a burst's buffers are not kept for good: every connection would keep its largest write (a stalled link's
        // queue, the hello's peer list: about 100 KB at 2000 players, 256 MB in all); what free roam writes a tick
        // stays far under these
        if buf.capacity() > KEEP_BUF {
            buf = Vec::new();
        }
        if body.capacity() > KEEP_BUF {
            body = Vec::new();
        }
        if items.capacity() > KEEP_ITEMS {
            items = Vec::new();
        }
    }
}

#[derive(Default)]
struct Encoded {
    /// what the items taken had added to `buffered`
    counted: usize,
    /// the datagrams' bytes on the wire (they were counted in `dgram`)
    extra: usize,
    close: bool,
    end: bool,
}

/// A page that takes compact bundles: the address it holds, the last bodies it was sent when it takes deltas (and the
/// last car states, when it takes them by their fields), and the writer's buffer for the bundle being made.
pub(crate) struct Compact<'a> {
    own: u32,
    refs: Option<&'a mut compact::Refs>,
    fp: Option<&'a mut crate::fpred::Fpred>,
    body: &'a mut Vec<u8>,
}

/// the items into `buf` in order, the datagrams as bundles (compact ones for a page that takes them) where the first
/// of them stands: what was queued before it goes first (a newcomer's welcome before its first datagrams: a page in
/// line said hello before its welcome, so its datagrams are bundles from the start, and a page that takes deltas keeps
/// the last bodies of every bundle it is sent; the datagrams were all written ahead of the batch's other items once);
/// nothing after a close frame
fn encode(stats: &Stats, items: &[Out], buf: &mut Vec<u8>, close_sent: &mut bool, compact: Option<Compact<'_>>) -> Encoded {
    let mut e = Encoded::default();
    let mut compact = Some(compact);
    for it in items {
        e.counted += it.wire_len();
        if matches!(it, Out::End) {
            e.end = true;
            continue;
        }
        if *close_sent {
            continue;
        }
        match it {
            Out::Dgram { .. } => {
                // (every datagram of the batch, once)
                if let Some(c) = compact.take() {
                    let start = buf.len();
                    match c {
                        Some(c) => bundle_compact(stats, items, buf, c),
                        None => bundle(stats, items, buf),
                    }
                    e.extra = buf.len() - start;
                }
            }
            Out::Text(b) => frame::encode(frame::TEXT, b, buf),
            Out::Raw(b) => frame::encode(frame::BINARY, b, buf),
            Out::Frame { data, src, dst } => one_frame(buf, data, *src, *dst),
            Out::Ping(p) => frame::encode(frame::PING, p, buf),
            Out::Pong(b) => frame::encode(frame::PONG, b, buf),
            Out::Close(p) => {
                frame::encode(frame::CLOSE, p, buf);
                *close_sent = true;
                e.close = true;
            }
            Out::End => {}
        }
    }
    e
}

/// a frame with its source and destination written in (the one copy a datagram gets); bytes 1-3 are reserved: 0 on
/// the wire, whatever the switch's copy holds there (compact.rs `mark`)
pub(crate) fn with_addrs(buf: &mut Vec<u8>, data: &[u8], src: u32, dst: u32) {
    buf.extend_from_slice(&[data[0], 0, 0, 0]);
    buf.extend_from_slice(&src.to_be_bytes());
    buf.extend_from_slice(&dst.to_be_bytes());
    buf.extend_from_slice(&data[12..]);
}

fn one_frame(buf: &mut Vec<u8>, data: &[u8], src: u32, dst: u32) {
    frame::header(frame::BINARY, data.len(), buf);
    with_addrs(buf, data, src, dst);
}

/// the datagrams as bundles of at most BUNDLE_OUT (entries of n = 0: each with its destination in it); one alone
/// goes as it is
fn bundle(stats: &Stats, items: &[Out], buf: &mut Vec<u8>) {
    let mut dgrams = items.iter().filter_map(|it| match it {
        Out::Dgram { data, src, dst, .. } => Some((data, *src, *dst)),
        _ => None,
    });
    let Some(mut next) = dgrams.next() else { return };
    let mut chunk: smallvec::SmallVec<[(&Bytes, u32, u32); 32]> = smallvec::SmallVec::new();
    loop {
        // a bundle takes datagrams while it stays within BUNDLE_OUT (the one that would not fit begins the next)
        chunk.clear();
        let mut sum = 0;
        let mut rest = None;
        loop {
            let l = 4 + next.0.len();
            if !chunk.is_empty() && sum + l > BUNDLE_OUT {
                rest = Some(next);
                break;
            }
            sum += l;
            chunk.push(next);
            match dgrams.next() {
                Some(d) => next = d,
                None => break,
            }
        }
        if chunk.len() == 1 {
            let (d, src, dst) = chunk[0];
            one_frame(buf, d, src, dst);
        } else {
            frame::header(frame::BINARY, 4 + sum, buf);
            buf.extend_from_slice(&[KIND_BUNDLE, 0, 0, 0]);
            for (d, src, dst) in &chunk {
                let len = d.len() as u16;
                buf.extend_from_slice(&[0, 0]);
                buf.extend_from_slice(&len.to_be_bytes());
                with_addrs(buf, d, *src, *dst);
            }
            stats.bundles_out.inc();
        }
        match rest {
            Some(d) => next = d,
            None => return,
        }
    }
}

/// the datagrams as compact bundles (compact.rs) of BUNDLE_OUT at most, a datagram alone in one too (its 16-byte
/// header costs more than a bundle's 4 and an entry's 5)
fn bundle_compact(stats: &Stats, items: &[Out], buf: &mut Vec<u8>, c: Compact<'_>) {
    let Compact { own, mut refs, mut fp, body } = c;
    body.clear();
    body.extend_from_slice(&[compact::KIND_COMPACT, 0, 0, 0]);
    let mut n = 0;
    let (now, mut moved) = (now_us(), [0u8; compact::TIMED_MAX]);
    for it in items {
        let Out::Dgram { data, src, dst, at: queued } = it else { continue };
        let data = compact::waited(data, if *queued != 0 { now.wrapping_sub(*queued) } else { 0 }, &mut moved);
        let at = body.len();
        compact::entry_fp(body, data, *src, *dst, own, refs.as_deref_mut(), fp.as_deref_mut());
        stats.entry_out(data, &body[at..]);
        // a bundle takes datagrams while it stays within BUNDLE_OUT (the one that would not fit begins the next; it
        // was written as the next one, in order, so the page reads the same entries the writer made)
        if n > 0 && body.len() > BUNDLE_OUT {
            compact_out(stats, buf, &body[..at]);
            body.drain(4..at);
            n = 0;
        }
        n += 1;
    }
    if n > 0 {
        compact_out(stats, buf, body);
    }
}

/// the switch's clock for a datagram's wait for the bundle clock: µs since its first reading, wrapping
pub(crate) fn now_us() -> u32 {
    static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    START.get_or_init(std::time::Instant::now).elapsed().as_micros() as u32
}

fn compact_out(stats: &Stats, buf: &mut Vec<u8>, bundle: &[u8]) {
    frame::header(frame::BINARY, bundle.len(), buf);
    buf.extend_from_slice(bundle);
    stats.bundles_out.inc();
}

const SHARDS: usize = 16;

/// one of the clock's lists, on a cache line of its own
#[repr(align(128))]
struct Shard(Mutex<Vec<Arc<Conn>>>);

/// The bundle clock: the writers of the peers with datagrams waiting, woken every tick.  Its lists are sharded by
/// connection, so that senders to different receivers rarely meet on one lock, and each shard ticks on a task of its
/// own, at a phase of its own: the wake-ups of a crowd's writers are spread over the tick and over the runtime's
/// workers (one task waking them all put them all on its own worker, for the others to steal).  Its tick can change
/// while it runs (the switch's `bundle_tick`).  Each tick is the period give or take `jitter` of it, at random: the game
/// takes the smallest delay of a friend's snapshots in 2 s for the path's (roam_core.c off_estimate) and draws a solid
/// car where its driver is now from it, and a tick as long as the time between those snapshots (50 ms, roam_rate 20)
/// made each wait about as long as the one before, so the car came out behind by that wait (roam-pair, 2026-10-24: up
/// to 14 ms, 0.22 m off on average).  A tick that wanders gives each snapshot another wait, and the smallest of 2 s
/// is about none.
pub(crate) struct Clock {
    shards: Box<[Shard]>,
    period_us: AtomicU64,
    jitter: f64,
}

impl Clock {
    pub(crate) fn new(period: Duration, jitter: f64) -> Self {
        let shards = (0..SHARDS).map(|_| Shard(Mutex::new(Vec::new()))).collect();
        let jitter = if jitter.is_finite() { jitter.clamp(0.0, 0.9) } else { 0.0 };
        Clock { shards, period_us: AtomicU64::new(period.as_micros() as u64), jitter }
    }

    /// the next tick's length: the period, give or take `jitter` of it (`rng` a shard's own xorshift state)
    fn next_period(&self, rng: &mut u64) -> Duration {
        let p = self.period();
        if self.jitter <= 0.0 {
            return p;
        }
        *rng ^= *rng << 13;
        *rng ^= *rng >> 7;
        *rng ^= *rng << 17;
        let u = (*rng >> 11) as f64 / (1u64 << 53) as f64;
        p.mul_f64(1.0 + self.jitter * (2.0 * u - 1.0))
    }

    pub(crate) fn period(&self) -> Duration {
        Duration::from_micros(self.period_us.load(Relaxed).max(1))
    }

    pub(crate) fn set_period(&self, d: Duration) {
        self.period_us.store(d.as_micros() as u64, Relaxed);
    }

    fn arm(&self, conn: &Arc<Conn>) {
        self.shards[conn.id as usize % SHARDS].0.lock().push(conn.clone());
    }

    /// every armed writer of one shard woken (its flag cleared first: a datagram queued after it took its queue arms
    /// it again)
    fn tick(&self, shard: usize, spare: &mut Vec<Arc<Conn>>) {
        std::mem::swap(&mut *self.shards[shard].0.lock(), spare);
        for c in spare.drain(..) {
            c.out.armed.store(false, Release);
            c.out.wake.notify_one();
        }
    }

    /// its tasks, until the switch is gone
    pub(crate) fn start(me: Weak<Clock>) {
        let Some(period) = me.upgrade().map(|c| c.period()) else { return };
        for shard in 0..SHARDS {
            let me = me.clone();
            let phase = period * shard as u32 / SHARDS as u32;
            let seed = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_nanos() as u64);
            let mut rng = (seed ^ (shard as u64 + 1).wrapping_mul(0x9E37_79B9_7F4A_7C15)) | 1;
            tokio::spawn(async move {
                let mut next = tokio::time::Instant::now() + phase;
                let mut spare = Vec::new();
                loop {
                    tokio::time::sleep_until(next).await;
                    let Some(c) = me.upgrade() else { return };
                    c.tick(shard, &mut spare);
                    // a tick that came late does not make the next ones come sooner
                    next = (next + c.next_period(&mut rng)).max(tokio::time::Instant::now());
                }
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dg(len: usize, dst: u32) -> Out {
        let mut d = vec![1u8; len];
        d[8..12].copy_from_slice(&0xFFFF_FFFFu32.to_be_bytes());
        Out::Dgram { data: Bytes::from(d), src: 0x0A4D_0002, dst, at: 0 }
    }

    #[test]
    fn a_tick_wanders_within_its_jitter_and_keeps_its_mean() {
        let c = Clock::new(Duration::from_millis(50), 0.4);
        let mut rng = 0x1234_5678_9ABC_DEF1u64;
        let ticks: Vec<Duration> = (0..10_000).map(|_| c.next_period(&mut rng)).collect();
        assert!(ticks.iter().all(|t| (Duration::from_millis(30)..=Duration::from_millis(70)).contains(t)));
        let mean = ticks.iter().sum::<Duration>() / ticks.len() as u32;
        assert!((Duration::from_micros(49_500)..=Duration::from_micros(50_500)).contains(&mean), "{mean:?}");
        assert!(ticks.iter().any(|t| *t < Duration::from_millis(32)) && ticks.iter().any(|t| *t > Duration::from_millis(68)));
        assert_eq!(Clock::new(Duration::from_millis(50), 0.0).next_period(&mut rng), Duration::from_millis(50));
        assert_eq!(Clock::new(Duration::from_millis(50), f64::NAN).next_period(&mut rng), Duration::from_millis(50));
    }

    #[test]
    fn header_lengths_match_rfc6455() {
        for len in [0, 1, 125, 126, 1000, 65535, 65536, 100_000] {
            let mut b = Vec::new();
            frame::header(frame::BINARY, len, &mut b);
            assert_eq!(b.len(), header_len(len), "{len}");
        }
    }

    #[test]
    fn datagrams_become_bundles_of_at_most_bundle_out_one_alone_as_it_is() {
        let stats = Stats::default();
        // one alone: as it is, its addresses written in
        let mut buf = Vec::new();
        let mut cs = false;
        let e = encode(&stats, &[dg(20, 0x0A4D_0103)], &mut buf, &mut cs, None);
        assert_eq!(e.extra, buf.len());
        assert_eq!(buf[0], 0x82);
        assert_eq!(buf[1] as usize, 20);
        assert_eq!(&buf[2 + 4..2 + 8], &0x0A4D_0002u32.to_be_bytes());
        assert_eq!(&buf[2 + 8..2 + 12], &0x0A4D_0103u32.to_be_bytes());
        assert_eq!(stats.bundles_out.get(), 0);
        // 8 KB datagrams: four to a bundle (4 x 8004 bytes fit in 32 KB), then the fifth alone
        let items: Vec<Out> = (0..5).map(|i| dg(8000, i)).collect();
        buf.clear();
        encode(&stats, &items, &mut buf, &mut cs, None);
        assert_eq!(stats.bundles_out.get(), 1);
        assert_eq!(buf[0], 0x82);
        assert_eq!(buf[1], 126);
        let len = u16::from_be_bytes([buf[2], buf[3]]) as usize;
        assert_eq!(len, 4 + 4 * 8004);
        assert_eq!(buf[4], KIND_BUNDLE);
        let second = 4 + len;
        assert_eq!(u16::from_be_bytes([buf[second + 2], buf[second + 3]]), 8000);
        assert_eq!(&buf[second + 4 + 8..second + 4 + 12], &4u32.to_be_bytes());
        assert_eq!(buf.len(), second + 4 + 8000);
        // 4 x 8192 + 4 x 4 is over 32 KB: three, then one alone
        let items: Vec<Out> = (0..4).map(|i| dg(8192, i)).collect();
        buf.clear();
        encode(&stats, &items, &mut buf, &mut cs, None);
        assert_eq!(stats.bundles_out.get(), 2);
        assert_eq!(u16::from_be_bytes([buf[2], buf[3]]) as usize, 4 + 3 * 8196);
    }

    // a newcomer's welcome, queued before its first datagrams, goes before their bundle (a page in line takes bundles
    // from its first moment); what came after the first datagram goes after the bundle; after a close frame nothing
    #[test]
    fn the_welcome_goes_before_the_first_bundle() {
        let stats = Stats::default();
        let (mut buf, mut cs) = (Vec::new(), false);
        let items = vec![Out::Text(Bytes::from_static(b"w")), dg(20, 1), Out::Text(Bytes::from_static(b"p")), dg(20, 2)];
        encode(&stats, &items, &mut buf, &mut cs, None);
        assert_eq!(&buf[..3], &[0x81, 1, b'w']);
        assert_eq!(&buf[3..5], &[0x82, 4 + 2 * 24]);
        assert_eq!(buf[5], KIND_BUNDLE);
        assert_eq!(&buf[5 + 4 + 2 * 24..], &[0x81, 1, b'p']);
        buf.clear();
        let items = vec![Out::Close(Bytes::from_static(&[3, 0xE8])), dg(20, 1)];
        encode(&stats, &items, &mut buf, &mut false, None);
        assert_eq!(buf, [0x88, 2, 3, 0xE8], "nothing after the close frame");
    }

    #[test]
    fn nothing_goes_after_a_close_frame() {
        let stats = Stats::default();
        let mut buf = Vec::new();
        let mut cs = false;
        let items =
            vec![Out::Text(Bytes::from_static(b"a")), Out::Close(Bytes::from_static(&[3, 0xF5])), Out::Text(Bytes::from_static(b"b"))];
        let e = encode(&stats, &items, &mut buf, &mut cs, None);
        assert!(e.close && cs);
        assert_eq!(buf, [0x81, 1, b'a', 0x88, 2, 3, 0xF5]);
        assert_eq!(e.counted, 3 + 4 + 3);
    }
}
