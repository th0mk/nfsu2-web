//! One connection: what Node's http server did per socket (`connectionListenerInternal`, `parserOnIncoming`,
//! `resOnFinish`, `checkConnections`, `socketOnTimeout`) with the server's rules on it (the pipelining cut, the
//! progress rule, the refusals of what it cannot read).  One task: it answers the requests in turn and, while the
//! service answers one, goes on reading and parsing what comes after it (Node did), so the pipelining cut and the
//! timers see what Node saw.
//!
//! The order of things follows Node's: a request is handed to the service as soon as its head is in, and the service
//! runs (until it waits on something) before the parser reads on, as Node's request handler ran inside the parser's
//! call.  A request the parser refuses after others: those before it are answered (Node: the ones it had answered at
//! once), then the refusal; a refusal while the service has written nothing of its answer yet cancels that answer
//! (Node wrote its refusal into the socket then, and the answer never went out).

use std::collections::VecDeque;
use std::future::Future;
use std::io;
use std::mem::MaybeUninit;
use std::pin::{Pin, pin};
use std::sync::Arc;
use std::task::Poll;
use std::time::Duration;

use bytes::{Buf, Bytes, BytesMut};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWriteExt, ReadBuf};
use tokio::sync::mpsc;
use tokio::time::{Instant, sleep_until};

use crate::head::has_token;
use crate::parse::{BodyKind, Event, Head, Parser};
use crate::response::{Flow, Meta};
use crate::{Body, ClientError, ConnInfo, Limits, ReadHalf, Request, Response, Service, Stream, Upgraded, Version, WriteHalf};

/// a read takes up to this much at once
const READ_CHUNK: usize = 16 << 10;
/// Node 24 keeps an idle keep-alive connection a second longer than it advertises
/// (HTTP_SERVER_KEEP_ALIVE_TIMEOUT_BUFFER: a client that reuses it at the advertised time finds it open)
const KEEP_ALIVE_BUFFER: Duration = Duration::from_secs(1);
/// a refusal: the raw answer, then the connection goes once the client closed, or after 1 s
const LINGER: Duration = Duration::from_secs(1);
/// body chunks waiting for the service (16 KiB each at most): then the connection reads no more until it takes them
const BODY_QUEUE: usize = 4;

/// What a request's body reader receives.
pub(crate) enum BodyMsg {
    Data(Bytes),
    End,
}

#[derive(Copy, Clone, PartialEq, Eq)]
enum Kind {
    Plain,
    /// Expect: 100-continue: "100 Continue", then the request
    Continue,
    /// any other Expect on HTTP/1.1 (Node's checkExpectation: the service's `expectation`, never its `request`)
    Expectation,
}

struct Pending {
    req: Request,
    meta: Meta,
    kind: Kind,
}

/// What the parser read, in order, for the answers to take in turn.
enum Item {
    Request(Box<Pending>),
    Upgrade(Box<Request>),
    /// CONNECT (Node has no listener for it): the connection goes without an answer
    Close,
    Error(ClientError),
}

/// Why the connection stops now.
enum Stop {
    /// at once, without a byte more (Node's socket.destroy())
    Destroy,
    /// with the service's raw answer for this (`Service::client_error`)
    Refuse(ClientError),
}

enum After {
    Next,
    Close,
    Destroy,
    Refuse(ClientError),
}

enum Ev {
    Served,
    Parse,
    Read(io::Result<usize>),
    /// a body chunk went to its reader; the body's channel is done (its end went, or its reader is gone)
    Permit(bool),
    Timer,
    /// the answer under way is another than when the timer was set (its first byte went, or it completed)
    Moved,
}

/// the progress rule (`progress`) for the answer under way
struct Prog {
    t0: Instant,
    mark: u64,
    at: Instant,
    next: Instant,
}

pub(crate) struct Conn {
    /// the connection's shares (lib.rs `Hold`): an upgrade hands them on
    hold: crate::Hold,
    r: ReadHalf,
    rbuf: BytesMut,
    read_cap: usize,
    parser: Parser,
    queue: VecDeque<Item>,
    /// requests read whose answers are not complete (what the pipelining cut counts)
    counted: usize,
    /// the answer under way counts in `counted` (Node's checkExpectation answers never did)
    in_flight_counted: bool,
    /// ...and its completion was taken into account already (`body_read`)
    uncounted: bool,
    /// the answer under way closed the connection before the parser read on: nothing more is written
    ended_early: bool,
    eof: bool,
    eof_seen: bool,
    limits: Arc<Limits>,
    info: ConnInfo,
    keep_alive: Duration,
    /// the listener's start: Node checks the header and request times of all its connections on one interval
    epoch: Instant,
    last_read: Instant,
    /// the socket's timeout (server.setTimeout, or the keep-alive time after an answer)
    sock_timeout: Duration,
    keep_alive_set: bool,
    /// when the request being read began (or the connection: Node counts from there until the first byte)
    msg_start: Option<Instant>,
    headers_done: bool,
    /// the body the parser is reading, to its request's reader
    body: Option<mpsc::Sender<BodyMsg>>,
    /// a chunk its reader has no room for yet: the parser waits
    body_pending: Option<BodyMsg>,
    flow: Arc<Flow>,
    prog: Option<Prog>,
}

/// One connection, after TLS: its requests, until it goes.
pub(crate) async fn serve<S: Service>(
    stream: Stream,
    info: ConnInfo,
    limits: Arc<Limits>,
    keep_alive: Duration,
    epoch: Instant,
    service: &S,
    hold: crate::Hold,
) {
    let (r, w) = stream.split();
    let flow = Flow::new();
    let res = Response::new(w, flow.clone());
    let now = Instant::now();
    let conn = Conn {
        hold,
        r,
        rbuf: BytesMut::new(),
        read_cap: limits.max_header_size + (64 << 10),
        parser: Parser::new(limits.max_header_size),
        queue: VecDeque::new(),
        counted: 0,
        in_flight_counted: false,
        uncounted: false,
        ended_early: false,
        eof: false,
        eof_seen: false,
        sock_timeout: limits.socket_idle,
        keep_alive_set: false,
        limits,
        info,
        keep_alive,
        epoch,
        last_read: now,
        msg_start: Some(now),
        headers_done: false,
        body: None,
        body_pending: None,
        flow,
        prog: None,
    };
    conn.run(service, res).await
}

impl Conn {
    async fn run<S: Service>(mut self, service: &S, mut res: Response) {
        loop {
            let item = match self.next(&mut res).await {
                Ok(Some(item)) => item,
                Ok(None) => return close(&mut res).await,
                Err(Stop::Destroy) => return,
                Err(Stop::Refuse(e)) => return self.refuse(service, &mut res, e).await,
            };
            match item {
                Item::Request(p) => match self.answer(service, &mut res, *p).await {
                    After::Next => {}
                    After::Close => return close(&mut res).await,
                    After::Destroy => return,
                    After::Refuse(e) => return self.refuse(service, &mut res, e).await,
                },
                Item::Upgrade(req) => return self.upgrade(service, res, *req).await,
                Item::Close => return,
                Item::Error(e) => return self.refuse(service, &mut res, e).await,
            }
        }
    }

    /// The next thing to answer: what the parser has read already, or what comes.  None: the client ended its side
    /// and everything was answered.  (`res`: the answers' writer, between two answers.)
    async fn next(&mut self, res: &mut Response) -> Result<Option<Item>, Stop> {
        let mut sleep = pin!(sleep_until(far()));
        let mut alarm = Alarm::new();
        loop {
            if let Some(item) = self.queue.pop_front() {
                return Ok(Some(item));
            }
            if self.can_parse() {
                self.parse(None)?;
                continue;
            }
            self.check_eof(None)?;
            if !self.queue.is_empty() {
                continue;
            }
            if self.eof && self.body_pending.is_none() {
                return Ok(None);
            }
            if self.rbuf.is_empty() {
                // waiting with nothing read: no read buffer held (thousands of idle keep-alive connections; one read
                // to its end has no capacity left, but its memory is still held), and none of the larger ones the
                // last exchange may have grown (`read_into` takes one when bytes come)
                self.rbuf = BytesMut::new();
                self.parser.idle();
                res.idle();
            }
            let read = self.can_read();
            let pend = self.body_pending.is_some();
            alarm.arm(sleep.as_mut(), self.deadline());
            let ev = tokio::select! {
                biased;
                r = read_into(&mut self.r, &mut self.rbuf), if read => Ev::Read(r),
                p = reserve(&self.body), if pend => Ev::Permit(send_pending(p, &mut self.body_pending)),
                () = &mut sleep => Ev::Timer,
            };
            self.on(ev, None)?;
        }
    }

    /// One request: the service answers it while the connection reads on.
    async fn answer<S: Service>(&mut self, service: &S, res: &mut Response, p: Pending) -> After {
        let Pending { req, meta, kind } = p;
        res.begin(meta);
        self.in_flight_counted = kind != Kind::Expectation;
        self.uncounted = false;
        if kind == Kind::Continue {
            // Node's writeContinue, before the request goes to the server
            if !matches!(tokio::time::timeout(self.limits.progress, res.raw(b"HTTP/1.1 100 Continue\r\n\r\n")).await, Ok(Ok(()))) {
                return After::Destroy;
            }
        }
        let driven = match kind {
            Kind::Expectation => self.drive(service.expectation(req, res)).await,
            _ => self.drive(service.request(req, res)).await,
        };
        match driven {
            Ok(()) => {}
            Err(Stop::Destroy) => return After::Destroy,
            Err(Stop::Refuse(e)) => return After::Refuse(e),
        }
        if !res.ended() {
            // a service that returned without ending its answer: ended here (the framing completed), or, with no
            // answer begun, the connection goes
            if !res.given() || self.flow.destroyed() {
                return After::Destroy;
            }
            if !matches!(tokio::time::timeout(self.limits.progress, res.end()).await, Ok(Ok(()))) {
                return After::Destroy;
            }
        }
        if self.flow.destroyed() {
            return After::Destroy;
        }
        if self.in_flight_counted && !self.uncounted {
            self.counted = self.counted.saturating_sub(1);
        }
        self.in_flight_counted = false;
        if res.last() {
            // a refusal read after an answer that closes the connection is still written (Node wrote it while the
            // answer's end was pending), unless the parser had read body bytes in between (Node's 'finish' ran then)
            if let (Some(Item::Error(e)), false) = (self.queue.front(), self.ended_early) {
                return After::Refuse(*e);
            }
            return After::Close;
        }
        if self.queue.is_empty() && !self.keep_alive.is_zero() {
            // Node's keep-alive time, from the end of the answer, until the next request's head is in
            self.sock_timeout = self.keep_alive.saturating_add(KEEP_ALIVE_BUFFER);
            self.keep_alive_set = true;
            self.last_read = Instant::now();
        }
        After::Next
    }

    /// The service's future, polled first each round, and the connection's reading, parsing and timers around it.
    async fn drive<F: Future<Output = ()>>(&mut self, fut: F) -> Result<(), Stop> {
        let mut fut = pin!(fut);
        let mut served = false;
        let mut sleep = pin!(sleep_until(far()));
        let mut alarm = Alarm::new();
        let flow = self.flow.clone();
        loop {
            if self.flow.destroyed() {
                return Err(Stop::Destroy);
            }
            let parse = self.can_parse();
            if served && !parse {
                // (what was read meanwhile is parsed before the answer stops counting: Node's 'finish' came after)
                return self.check_eof(Some(true));
            }
            let read = self.can_read();
            let pend = self.body_pending.is_some();
            alarm.arm(sleep.as_mut(), self.deadline());
            let armed = flow.under_way();
            let ev = tokio::select! {
                biased;
                () = &mut fut, if !served => Ev::Served,
                // the answer's first byte went (or it completed) while the service ran: the progress checks start
                // from then, and the timer must be set for them now (a file's answer starts once the file is open;
                // nothing else may come to set it, a client that stopped reading sends nothing)
                () = moved(&flow, armed), if !served => Ev::Moved,
                () = std::future::ready(()), if parse => Ev::Parse,
                r = read_into(&mut self.r, &mut self.rbuf), if read => Ev::Read(r),
                p = reserve(&self.body), if pend => Ev::Permit(send_pending(p, &mut self.body_pending)),
                () = &mut sleep => Ev::Timer,
            };
            match ev {
                Ev::Served => served = true,
                Ev::Parse => self.parse(Some(served))?,
                ev => self.on(ev, Some(served))?,
            }
        }
    }

    /// `drive`: None while the connection waits for a request; Some(served) while one is answered.
    fn on(&mut self, ev: Ev, drive: Option<bool>) -> Result<(), Stop> {
        match ev {
            Ev::Read(Ok(0)) => {
                self.eof = true;
                self.check_eof(drive)
            }
            Ev::Read(Ok(_)) => {
                self.last_read = Instant::now();
                Ok(())
            }
            Ev::Read(Err(_)) => Err(Stop::Destroy),
            Ev::Permit(done) => {
                if done {
                    self.body = None;
                    self.body_pending = None;
                }
                Ok(())
            }
            Ev::Timer => self.timers(drive),
            Ev::Served | Ev::Parse | Ev::Moved => Ok(()),
        }
    }

    fn can_parse(&self) -> bool {
        !self.parser.stopped() && self.body_pending.is_none() && (!self.rbuf.is_empty() || self.parser.pending()) && !self.queue_full()
    }

    /// What the parser has read and the answers have not taken yet is bounded too: `pipelined` items, then the next
    /// request is read only as they are taken (the one being read is read to its end, its timers as Node's).  (The
    /// pipelining cut counts the requests the service's `request` sees; Node's checkExpectation answers never counted,
    /// so behind an answer under way a client could queue those without end, each a whole Request in memory.)  A
    /// client within the cut never meets this: it is cut first.
    fn queue_full(&self) -> bool {
        self.queue.len() >= self.limits.pipelined.max(1) && self.parser.between()
    }

    fn can_read(&self) -> bool {
        !self.eof && self.rbuf.len() < self.read_cap
    }

    /// Parses what was read.  While nothing is being answered, it stops at the first request (to answer it at once).
    fn parse(&mut self, drive: Option<bool>) -> Result<(), Stop> {
        while self.can_parse() {
            let (n, ev) = self.parser.parse(&self.rbuf);
            match ev {
                Event::More => self.rbuf.advance(n),
                Event::Begin => {
                    self.rbuf.advance(n);
                    self.msg_start = Some(Instant::now());
                    self.headers_done = false;
                }
                Event::Head(h) => {
                    self.rbuf.advance(n);
                    self.on_head(*h)?;
                    if drive.is_none() {
                        return Ok(());
                    }
                }
                Event::Body(len) => {
                    if drive.is_some() && len > 0 {
                        self.body_read();
                    }
                    if self.body.is_some() {
                        let mut taken = self.rbuf.split_to(n);
                        let data = taken.split_off(n - len).freeze();
                        self.deliver(BodyMsg::Data(data));
                    } else {
                        // nobody reads it (Node's req._dump())
                        self.rbuf.advance(n);
                    }
                }
                Event::End => {
                    self.rbuf.advance(n);
                    self.msg_start = None;
                    self.deliver(BodyMsg::End);
                }
                Event::Error(e) => {
                    self.rbuf.advance(n);
                    return self.fail(e, drive);
                }
            }
        }
        Ok(())
    }

    /// The client ended its side and everything it sent was read: in the middle of a request that is Node's 400
    /// ("Invalid EOF state"); between requests nothing (the connection goes once they are answered).
    fn check_eof(&mut self, drive: Option<bool>) -> Result<(), Stop> {
        if !self.eof || self.eof_seen || !self.rbuf.is_empty() || self.body_pending.is_some() || self.parser.pending() {
            return Ok(());
        }
        self.eof_seen = true;
        if self.parser.stopped() {
            return Ok(());
        }
        match self.parser.eof_error() {
            Some(e) => self.fail(e, drive),
            None => Ok(()),
        }
    }

    fn on_head(&mut self, h: Head) -> Result<(), Stop> {
        self.headers_done = true;
        if self.keep_alive_set {
            // Node's resetSocketTimeout: back to the server's timeout once a request's head is in
            self.keep_alive_set = false;
            self.sock_timeout = self.limits.socket_idle;
            self.last_read = Instant::now();
        }
        let method = h.method_str();
        if h.close {
            // CONNECT: Node has no listener for it, the connection goes
            self.parser.stop();
            self.msg_start = None;
            self.queue.push_back(Item::Close);
            return Ok(());
        }
        // PRI read as HTTP/0.9: Node handed it to the server and read on, then destroyed the connection (one that
        // keeps alive goes after its answer; one that does not goes anyway)
        let close_after = h.pri && h.keep_alive;
        let (major, minor, keep_alive, upgrade, kind_body) = (h.major, h.minor, h.keep_alive, h.upgrade, h.body);
        let Head { url, headers, .. } = h;
        let mut req = Request {
            method: method.to_string(),
            url,
            version: if (major, minor) == (1, 0) { Version::Http10 } else { Version::Http11 },
            headers,
            conn: self.info.clone(),
            body: Body { rx: None, length: None },
            major,
            minor,
        };
        if upgrade {
            // the rest of the connection is the new protocol's (the body, if one was announced, too)
            self.parser.stop();
            self.msg_start = None;
            self.queue.push_back(Item::Upgrade(Box::new(req)));
            return Ok(());
        }
        // Node: Expect is looked at only on HTTP/1.1
        let kind = match req.headers.get("expect") {
            Some(v) if (major, minor) == (1, 1) => {
                if has_token(v, "100-continue") {
                    Kind::Continue
                } else {
                    Kind::Expectation
                }
            }
            _ => Kind::Plain,
        };
        if kind != Kind::Expectation {
            // requests on one connection without a finished answer; past `pipelined` the connection goes (each queued
            // answer may hold an open file and its first buffer: a client that queued thousands made the Node server
            // hold gigabytes and as many open files)
            self.counted += 1;
            if self.counted > self.limits.pipelined {
                return Err(Stop::Destroy);
            }
        }
        if kind_body != BodyKind::None {
            let (tx, rx) = mpsc::channel(BODY_QUEUE);
            self.body = Some(tx);
            req.body = Body { rx: Some(rx), length: if let BodyKind::Length(n) = kind_body { Some(n) } else { None } };
        }
        let te_chunked = req.headers.get("te").is_some_and(|v| has_token(v, "chunked"));
        let meta = Meta {
            head: method == "HEAD",
            keep_alive,
            chunked_default: (major >= 1 && minor >= 1) || te_chunked,
            keep_alive_secs: (!self.keep_alive.is_zero()).then_some(self.keep_alive.as_secs()),
        };
        self.queue.push_back(Item::Request(Box::new(Pending { req, meta, kind })));
        if close_after {
            self.queue.push_back(Item::Close);
        }
        Ok(())
    }

    /// Body bytes were read while an answer is under way.  Node's parser hands body bytes to JavaScript through a
    /// call that runs what is due first: an answer completed by then has finished there (its 'finish': it stops
    /// counting toward `pipelined`, and an answer that closes the connection ends the socket, so a refusal read
    /// after it is never written).
    fn body_read(&mut self) {
        if !self.flow.done() {
            return;
        }
        if self.in_flight_counted && !self.uncounted {
            self.uncounted = true;
            self.counted = self.counted.saturating_sub(1);
        }
        if self.flow.closing() {
            self.ended_early = true;
        }
    }

    /// a body chunk (or its end) to the request's reader
    fn deliver(&mut self, msg: BodyMsg) {
        let Some(tx) = &self.body else { return };
        let end = matches!(msg, BodyMsg::End);
        match tx.try_send(msg) {
            Ok(()) => {
                if end {
                    self.body = None;
                }
            }
            Err(mpsc::error::TrySendError::Full(m)) => self.body_pending = Some(m),
            Err(mpsc::error::TrySendError::Closed(_)) => self.body = None,
        }
    }

    /// A request that cannot be read: it is refused after the answers before it, or at once if the answer being
    /// written has nothing on the wire yet.
    fn fail(&mut self, e: ClientError, drive: Option<bool>) -> Result<(), Stop> {
        self.parser.stop();
        self.msg_start = None;
        // the body that was coming is broken: its reader sees the channel close without its end
        self.body = None;
        self.body_pending = None;
        if drive == Some(false) && !self.flow.wire() {
            return Err(Stop::Refuse(e));
        }
        // the requests read after the answer under way go unanswered (Node's queued answers never got the socket)
        self.queue.clear();
        self.queue.push_back(Item::Error(e));
        Ok(())
    }

    fn timers(&mut self, drive: Option<bool>) -> Result<(), Stop> {
        let now = Instant::now();
        // the socket's own timeout: no answer, the connection goes (Node's socketOnTimeout destroys it)
        if !self.sock_timeout.is_zero() && now >= later(self.last_activity(), self.sock_timeout) {
            return Err(Stop::Destroy);
        }
        // the header and request times (slow-loris): 408
        if self.timeout_tick().is_some_and(|t| now >= t) {
            return self.fail(ClientError::Timeout, drive);
        }
        self.progress(now)
    }

    fn deadline(&self) -> Instant {
        let mut d = far();
        if !self.sock_timeout.is_zero() {
            d = d.min(later(self.last_activity(), self.sock_timeout));
        }
        if let Some(t) = self.timeout_tick() {
            d = d.min(t);
        }
        if let Some(t) = self.prog_next() {
            d = d.min(t);
        }
        d
    }

    /// reads and writes both count (Node's socket timeout restarts on either)
    fn last_activity(&self) -> Instant {
        self.last_read.max(Instant::from_std(self.flow.last_write()))
    }

    /// When Node's next check of the connections (every check_interval from the listener's start) finds the
    /// request being read too slow: its head not in within headers_timeout, or all of it within request_timeout.
    fn timeout_tick(&self) -> Option<Instant> {
        let start = self.msg_start?;
        if self.parser.stopped() {
            return None;
        }
        let (mut ht, mut rt) = (self.limits.headers_timeout, self.limits.request_timeout);
        if !rt.is_zero() && ht > rt {
            std::mem::swap(&mut ht, &mut rt);
        }
        let mut expiry: Option<Instant> = None;
        if !self.headers_done && !ht.is_zero() {
            expiry = Some(later(start, ht));
        }
        if !rt.is_zero() {
            let e = later(start, rt);
            expiry = Some(expiry.map_or(e, |x| x.min(e)));
        }
        Some(tick_after(self.epoch, self.limits.check_interval, expiry?))
    }

    fn prog_tick(&self) -> Duration {
        // Node checked every 2.5 s for its 20 s window
        (self.limits.progress / 8).max(Duration::from_millis(1))
    }

    fn prog_next(&self) -> Option<Instant> {
        let t0 = Instant::from_std(self.flow.under_way()?);
        Some(match &self.prog {
            Some(p) if p.t0 == t0 => p.next,
            _ => later(t0, self.prog_tick()),
        })
    }

    /// The progress rule: an answer under way must move at least progress_bytes in every `progress` and may take
    /// response_max in all, or the connection goes (a client that stops reading, or reads a trickle, would keep its
    /// download and the connection for good: Node's socket timeout does not fire while a write waits, and a trickle
    /// of 1 KB every 30 s resets it).  Together with the per-client budgets this keeps one client from holding the
    /// server's connections.
    fn progress(&mut self, now: Instant) -> Result<(), Stop> {
        let Some(t0) = self.flow.under_way().map(Instant::from_std) else {
            self.prog = None;
            return Ok(());
        };
        let tick = self.prog_tick();
        if self.prog.as_ref().is_none_or(|p| p.t0 != t0) {
            self.prog = Some(Prog { t0, mark: 0, at: t0, next: later(t0, tick) });
        }
        let Some(p) = self.prog.as_mut() else { return Ok(()) };
        while now >= p.next {
            let at = p.next;
            let bytes = self.flow.bytes();
            if bytes - p.mark >= self.limits.progress_bytes {
                p.mark = bytes;
                p.at = at;
            }
            // (>=: Node's checks, on a timer that only ever runs late, were always more than 20 s apart after 8 of
            // them, so a window without progress was cut at the 8th check, not the 9th)
            if at - p.at >= self.limits.progress || at - p.t0 > self.limits.response_max {
                return Err(Stop::Destroy);
            }
            p.next = later(p.next, tick);
        }
        Ok(())
    }

    async fn refuse<S: Service>(&mut self, service: &S, res: &mut Response, e: ClientError) {
        let Some(answer) = service.client_error(e) else { return };
        let Some(mut w) = res.take_io() else { return };
        refuse_io(&mut self.r, &mut w, &answer).await;
    }

    /// Node's 'upgrade': the connection and what was read past the head go to the service.  The HTTP side's buffers
    /// go first (an upgraded connection, the /lan WebSocket, lives for hours), and `pre` is a copy of its own size.
    async fn upgrade<S: Service>(self, service: &S, mut res: Response, req: Request) {
        let w = res.take_io();
        drop(res);
        let Conn { r, rbuf, parser, queue, hold, .. } = self;
        drop((parser, queue));
        let pre = Bytes::copy_from_slice(&rbuf);
        drop(rbuf);
        let Some(stream) = w.and_then(|w| reunite(r, w)) else { return };
        service.upgrade(req, Upgraded { stream, pre, hold }).await;
    }
}

/// the first of Node's checks (every `ci` from `epoch`) after `t`
fn tick_after(epoch: Instant, ci: Duration, t: Instant) -> Instant {
    if ci.is_zero() {
        return later(t, Duration::from_millis(1));
    }
    let since = t.saturating_duration_since(epoch).as_nanos();
    let k = since / ci.as_nanos() + 1;
    later(epoch, Duration::from_nanos(u64::try_from(k.saturating_mul(ci.as_nanos())).unwrap_or(u64::MAX / 2)))
}

fn far() -> Instant {
    later(Instant::now(), Duration::from_secs(86_400 * 365))
}

/// `t + d`, or far off where that does not fit (a limit of "forever")
fn later(t: Instant, d: Duration) -> Instant {
    t.checked_add(d).unwrap_or_else(|| t + Duration::from_secs(86_400 * 365 * 30))
}

/// The connection's one timer, set again only when what it waits for came closer or it went off (each read moves the
/// socket timeout's deadline later: the timer then goes off early once and finds nothing due).
struct Alarm {
    at: Instant,
}

impl Alarm {
    fn new() -> Alarm {
        Alarm { at: far() }
    }
    fn arm(&mut self, sleep: std::pin::Pin<&mut tokio::time::Sleep>, deadline: Instant) {
        if deadline < self.at || sleep.is_elapsed() {
            self.at = deadline;
            sleep.reset(deadline);
        }
    }
}

/// Ready once the answer under way is another than `armed`.  It never wakes the task itself: the answer changes only
/// while the service's future runs, and `drive` polls that just before this, in the same round.
fn moved(flow: &Flow, armed: Option<std::time::Instant>) -> impl Future<Output = ()> + '_ {
    std::future::poll_fn(move |_| if flow.under_way() != armed { std::task::Poll::Ready(()) } else { std::task::Poll::Pending })
}

/// One read into `buf`.  Into a buffer without room (none: the connection waits with nothing read) it goes through
/// the stack, and only what came is kept: a wait holds no buffer (reserving one first, as before, held 16 KiB on
/// every idle connection for as long as it waited).
async fn read_into(r: &mut ReadHalf, buf: &mut BytesMut) -> io::Result<usize> {
    if buf.capacity() == 0 {
        return read_fresh(r, buf).await;
    }
    if buf.capacity() - buf.len() < 1024 {
        buf.reserve(READ_CHUNK);
    }
    r.read_buf(buf).await
}

fn read_fresh<'a>(r: &'a mut ReadHalf, buf: &'a mut BytesMut) -> impl Future<Output = io::Result<usize>> + 'a {
    std::future::poll_fn(move |cx| {
        // (on the stack of this poll only, never in the connection's future)
        let mut tmp = [const { MaybeUninit::<u8>::uninit() }; READ_CHUNK];
        let mut rb = ReadBuf::uninit(&mut tmp);
        match Pin::new(&mut *r).poll_read(cx, &mut rb) {
            Poll::Ready(Ok(())) => {
                buf.extend_from_slice(rb.filled());
                Poll::Ready(Ok(rb.filled().len()))
            }
            Poll::Ready(Err(e)) => Poll::Ready(Err(e)),
            Poll::Pending => Poll::Pending,
        }
    })
}

/// room for one body message (None: the reader is gone)
async fn reserve(tx: &Option<mpsc::Sender<BodyMsg>>) -> Option<mpsc::Permit<'_, BodyMsg>> {
    match tx {
        Some(tx) => tx.reserve().await.ok(),
        None => None,
    }
}

/// the waiting body message into the room made for it: true when the body's channel is done (its end went, or its
/// reader is gone)
fn send_pending(permit: Option<mpsc::Permit<'_, BodyMsg>>, pending: &mut Option<BodyMsg>) -> bool {
    let Some(permit) = permit else { return true };
    match pending.take() {
        Some(m) => {
            let end = matches!(m, BodyMsg::End);
            permit.send(m);
            end
        }
        None => false,
    }
}

/// The connection goes after its last answer: the write side is closed (FIN, or TLS close_notify), then all of it.
async fn close(res: &mut Response) {
    if let Some(mut w) = res.take_io() {
        let _ = tokio::time::timeout(LINGER, w.shutdown()).await;
    }
}

/// A refusal: the raw answer, the write side closed, then the connection goes once the client closed its side, or
/// after a second (what the client sends meanwhile is read and dropped).
pub(crate) async fn refuse_io(r: &mut ReadHalf, w: &mut WriteHalf, answer: &[u8]) {
    let deadline = Instant::now() + LINGER;
    let wrote = tokio::time::timeout_at(deadline, async {
        w.write_all(answer).await?;
        w.flush().await?;
        w.shutdown().await
    })
    .await;
    if !matches!(wrote, Ok(Ok(()))) {
        return;
    }
    // (on the heap, for the refusal's second only: an array here would be part of every connection's future, the
    // idle ones' too)
    let mut buf = vec![0u8; 4096];
    let _ = tokio::time::timeout_at(deadline, async {
        loop {
            match r.read(&mut buf).await {
                Ok(0) | Err(_) => break,
                Ok(_) => {}
            }
        }
    })
    .await;
}

/// the two halves of a connection, whole again (for an upgrade)
pub(crate) fn reunite(r: ReadHalf, w: WriteHalf) -> Option<Stream> {
    match (r, w) {
        (ReadHalf::Plain(r), WriteHalf::Plain(w)) => r.reunite(w).ok().map(Stream::Plain),
        (ReadHalf::Tls(r), WriteHalf::Tls(w)) => Some(Stream::Tls(Box::new(r.unsplit(w)))),
        (ReadHalf::Unix(r), WriteHalf::Unix(w)) => r.reunite(w).ok().map(Stream::Unix),
        _ => None,
    }
}
