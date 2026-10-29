//! The tests' `World`: one online host and its browsers, frames out routed to fake games by address (lobby.rs,
//! audit.rs and the others).  Two manual clocks: `advance` moves the lobby's `now()` and runs its tick, `sleep` moves
//! the timer clock and runs what is due (as real time passing would: the timers fire, the lobby's `now` stays).

use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::rc::Rc;
use std::sync::Arc;

use nfsu2_util::{FrameCtx, SendResult};
use parking_lot::Mutex;

use crate::aries::{FieldMap, Fields, FrameReader, encode_frame, encode_frame_text};
use crate::buddy::BuddyLimits;
use crate::core::{Core, CoreOptions};
use crate::lobby::Limits;
use crate::vhost::{DATA, FIN, RST, SYN, SYNACK, VhostLimits, lan_frame};
use crate::{Clocks, PORT_BUDDY, PORT_LOBBY, VHOST_VIP};

pub fn ip(s: &str) -> u32 {
    s.split('.').fold(0u32, |a, b| (a << 8) | b.parse::<u32>().unwrap())
}

/// a message a fake game got
#[derive(Clone, Debug)]
pub struct M {
    pub kind: String,
    pub code: String,
    pub f: FieldMap,
}

impl M {
    pub fn get(&self, k: &str) -> Option<&str> {
        self.f.get(k)
    }
    pub fn g(&self, k: &str) -> &str {
        self.f.get(k).unwrap_or_else(|| panic!("{} has no {k}: {:?}", self.kind, self.f))
    }
}

/// one stream of a fake game
pub struct Conn {
    pub cid: u32,
    pub dport: u16,
    r: FrameReader,
    /// the bytes as they came (a test that reads a reply that is not lobby messages)
    pub raw: Vec<u8>,
    pub msgs: Vec<M>,
    pub open: bool,
    pub reset: bool,
    pub fin: bool,
    pub lkey: Option<String>,
}

impl Conn {
    pub fn last(&self, kind: &str) -> Option<&M> {
        self.msgs.iter().rev().find(|m| m.kind == kind)
    }
    pub fn l(&self, kind: &str) -> &M {
        self.last(kind).unwrap_or_else(|| panic!("no {kind} among {:?}", self.msgs.iter().map(|m| &m.kind).collect::<Vec<_>>()))
    }
    pub fn count(&self, kind: &str) -> usize {
        self.msgs.iter().filter(|m| m.kind == kind).count()
    }
    pub fn take(&mut self) -> Vec<M> {
        std::mem::take(&mut self.msgs)
    }
}

pub struct Client {
    pub group: Arc<str>,
    pub conns: HashMap<u16, Conn>,
    /// the keyphrase its page's hello named (FrameCtx::owner)
    pub owner: Option<Arc<str>>,
}

/// a stream: its browser and its port
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct C(pub u32, pub u16);

/// the lines logged: (line, key)
pub type Lines = Rc<RefCell<Vec<(String, Option<String>)>>>;
/// frames out: (browser, frame)
type Outbox = Arc<Mutex<Vec<(u32, Vec<u8>)>>>;
/// every frame the host sent: (browser, frame, refused)
pub type Sent = Arc<Mutex<Vec<(u32, Vec<u8>, bool)>>>;

pub struct World {
    pub core: Core,
    pub t: Rc<Cell<f64>>,
    pub tt: Rc<Cell<f64>>,
    pub lines: Lines,
    outbox: Outbox,
    /// browsers the fake switch refuses frames for (Full)
    pub slow: Arc<Mutex<HashSet<u32>>>,
    /// every frame the host sent, and whether the switch refused it
    pub sent: Sent,
    pub clients: HashMap<u32, Client>,
    /// the streams of a browser's fake game that a new one replaced: they keep what they got, and get no more
    orphans: HashMap<(u32, u16), Conn>,
    /// (the load measure) frames out are dropped, not read by the fake games
    pub mute: bool,
    next_port: u16,
    next_cid: u32,
}

#[derive(Default)]
pub struct Opts {
    pub limits: Limits,
    pub buddy: BuddyLimits,
    pub state_dir: Option<std::path::PathBuf>,
    /// the lobby's clock at the start (ms; None: 1,000,000)
    pub t0: Option<f64>,
    /// the claimed names (nfsu2-claims)
    pub claims: Option<Arc<nfsu2_claims::Claims>>,
}

impl World {
    pub fn new(o: Opts) -> World {
        let t = Rc::new(Cell::new(o.t0.unwrap_or(1_000_000.0)));
        let tt = Rc::new(Cell::new(0.0));
        let lines = Rc::new(RefCell::new(Vec::new()));
        let (t2, tt2, l2) = (t.clone(), tt.clone(), lines.clone());
        let mut core = Core::new(CoreOptions {
            state_dir: o.state_dir,
            log: Rc::new(move |s: &str, k: Option<&str>| l2.borrow_mut().push((s.to_string(), k.map(str::to_string)))),
            tap: None,
            claims: o.claims,
            clocks: Rc::new(Clocks { now: Box::new(move || t2.get()), timer: Box::new(move || tt2.get()) }),
            limits: o.limits,
            buddy_limits: o.buddy,
            vhost: VhostLimits::default(),
        });
        let outbox: Outbox = Arc::default();
        let slow: Arc<Mutex<HashSet<u32>>> = Arc::default();
        let sent: Sent = Arc::default();
        let (ob, sl, se) = (outbox.clone(), slow.clone(), sent.clone());
        core.attach(Arc::new(move |vip, f, _| {
            let refused = sl.lock().contains(&vip);
            se.lock().push((vip, f.to_vec(), refused));
            if refused {
                return SendResult::Full;
            }
            ob.lock().push((vip, f.to_vec()));
            SendResult::Sent
        }));
        World {
            core,
            t,
            tt,
            lines,
            outbox,
            slow,
            sent,
            clients: HashMap::new(),
            orphans: HashMap::new(),
            mute: false,
            next_port: 50000,
            next_cid: 1,
        }
    }

    /// the lines logged so far
    pub fn text(&self) -> Vec<String> {
        self.lines.borrow().iter().map(|(s, _)| s.clone()).collect()
    }
    pub fn logged(&self, part: &str) -> bool {
        self.lines.borrow().iter().any(|(s, _)| s.contains(part))
    }

    /// frames the host sent: to their fake games
    pub fn deliver(&mut self) {
        let frames = std::mem::take(&mut *self.outbox.lock());
        if self.mute {
            self.sent.lock().clear();
            return;
        }
        for (vip, f) in frames {
            let Some(c) = self.clients.get_mut(&vip) else { continue };
            let port = u16::from_be_bytes([f[14], f[15]]);
            let Some(conn) = c.conns.get_mut(&port) else { continue };
            match f[16] {
                SYNACK => conn.open = true,
                RST => conn.reset = true,
                FIN => conn.fin = true,
                DATA => {
                    conn.raw.extend_from_slice(&f[24..]);
                    for m in conn.r.push(&f[24..]) {
                        let parsed = FieldMap::parse_with(&m.body, |_| false);
                        conn.msgs.push(M { kind: m.kind, code: m.code, f: parsed });
                    }
                }
                _ => {}
            }
        }
    }

    /// `w.advance(ms)`: the lobby's clock moves, its tick runs
    pub fn advance(&mut self, ms: f64) {
        self.t.set(self.t.get() + ms);
        self.core.tick();
        self.deliver();
    }

    /// real time passes (on the timer clock): the timers due run
    pub fn sleep(&mut self, ms: f64) {
        let end = self.tt.get() + ms;
        // the clock stops at each timer's time (a timer set by one counts from then, as on a real clock)
        while let Some(due) = self.core.next_due().filter(|d| *d <= end) {
            self.tt.set(due.max(self.tt.get()));
            self.core.run_due(self.tt.get());
            self.deliver();
        }
        self.tt.set(end);
    }

    /// a browser at `vip` of client `group` (the lobby tests: the browser's own address stands for its client)
    pub fn client_of(&mut self, vip: &str, group: &str) -> u32 {
        let v = ip(vip);
        if let Some(old) = self.clients.insert(v, Client { group: group.into(), conns: HashMap::new(), owner: None }) {
            for (port, c) in old.conns {
                self.orphans.insert((v, port), c);
            }
        }
        v
    }
    pub fn client(&mut self, vip: &str) -> u32 {
        let v = ip(vip);
        self.client_of(vip, &format!("vip {v}"))
    }

    /// the browser's page plays with this keyphrase's token (its hello's "claim") from its next connection on
    pub fn set_owner(&mut self, vip: u32, token: &str) {
        self.clients.get_mut(&vip).expect("a client").owner = nfsu2_claims::owner_of_token(token);
    }

    pub fn raw(&mut self, vip: u32, frame: &[u8]) {
        let (group, owner) = (self.clients[&vip].group.clone(), self.clients[&vip].owner.clone());
        self.core.frame(vip, frame, &FrameCtx { group, site: None, cause: None, slot: None, owner });
        self.deliver();
    }

    fn out(&mut self, c: C, op: u8, data: &[u8]) {
        let (cid, dport) = {
            let k = self.c(c);
            (k.cid, k.dport)
        };
        self.raw(c.0, &lan_frame(4, c.0, VHOST_VIP, c.1, dport, op, cid, data));
    }

    pub fn connect(&mut self, vip: u32, dport: u16) -> C {
        let (port, cid) = (self.next_port, self.next_cid);
        self.next_port += 1;
        self.next_cid += 1;
        self.connect_as(vip, dport, port, cid)
    }

    /// a stream from a given port, with a given connection id
    pub fn connect_as(&mut self, vip: u32, dport: u16, port: u16, cid: u32) -> C {
        self.clients.get_mut(&vip).expect("a client").conns.insert(
            port,
            Conn {
                cid,
                dport,
                r: FrameReader::new(1 << 16),
                raw: Vec::new(),
                msgs: Vec::new(),
                open: false,
                reset: false,
                fin: false,
                lkey: None,
            },
        );
        let c = C(vip, port);
        self.out(c, SYN, &[]);
        c
    }

    pub fn send(&mut self, c: C, kind: &str, fields: Fields) {
        let f = encode_frame(kind, "", &fields).unwrap();
        self.out(c, DATA, &f);
    }
    pub fn send_text(&mut self, c: C, kind: &str, text: &str) {
        let f = encode_frame_text(kind, "", text).unwrap();
        self.out(c, DATA, &f);
    }
    pub fn send_raw(&mut self, c: C, data: &[u8]) {
        self.out(c, DATA, data);
    }
    pub fn close(&mut self, c: C) {
        self.out(c, FIN, &[]);
    }
    pub fn rst(&mut self, c: C) {
        self.out(c, RST, &[]);
    }

    pub fn c(&self, c: C) -> &Conn {
        self.clients.get(&c.0).and_then(|x| x.conns.get(&c.1)).or_else(|| self.orphans.get(&(c.0, c.1))).expect("a stream")
    }
    pub fn cm(&mut self, c: C) -> &mut Conn {
        match self.clients.get_mut(&c.0).and_then(|x| x.conns.get_mut(&c.1)) {
            Some(k) => k,
            None => self.orphans.get_mut(&(c.0, c.1)).expect("a stream"),
        }
    }

    /// a lobby session up to a persona (what the game does from the account screen to the Online Main Menu)
    pub fn login_as(&mut self, vip: u32, name: &str, persona: &str, pass: &str) -> C {
        let c = self.connect(vip, PORT_LOBBY);
        self.send(c, "addr", fields![("ADDR", "10.77.0.99"), ("PORT", c.1)]);
        self.send(c, "skey", fields![("SKEY", "$5075626c6963204b6579")]);
        self.send(c, "news", fields![("NAME", 7)]);
        self.send(c, "sele", fields![("MYGAME", 1), ("STATS", 5000), ("ASYNC", 1), ("MESGS", 1)]);
        self.send(
            c,
            "auth",
            fields![("NAME", name), ("PASS", pass), ("REGKEY", "AAAA-BBBB-CCCC"), ("PSES", "0123456789abcdef"), ("MID", "$024e564c0002")],
        );
        if persona != name {
            self.send(c, "cper", fields![("PERS", persona)]);
        }
        self.send(c, "pers", fields![("PERS", persona), ("MID", "$024e564c0002")]);
        let lkey = self.c(c).last("pers").and_then(|m| m.get("LKEY")).map(str::to_string);
        self.cm(c).lkey = lkey;
        c
    }
    pub fn login(&mut self, vip: u32, name: &str) -> C {
        self.login_as(vip, name, name, "secret")
    }

    /// an EA Messenger connection, logged in with a lobby session's key
    pub fn buddy(&mut self, vip: u32, lkey: &str) -> C {
        let c = self.connect(vip, PORT_BUDDY);
        self.send(
            c,
            "AUTH",
            fields![("PROD", "NFS-CONSOLE-2005"), ("VERS", "0.1"), ("PRES", 20920), ("USER", "/PC/NFS-CONSOLE-2005"), ("LKEY", lkey)],
        );
        c
    }
    pub fn lkey(&self, c: C) -> String {
        self.c(c).lkey.clone().unwrap_or_default()
    }

    pub fn peer_gone(&mut self, vip: u32) {
        self.core.peer_gone(vip);
        self.deliver();
    }

    /// the lobby's sessions as (persona) for the tests that count them
    pub fn personas(&self) -> Vec<Option<String>> {
        self.core.lobby.sessions.iter().map(|s| self.core.lobby.s(*s).persona.clone()).collect()
    }
}
