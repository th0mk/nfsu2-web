//! EA Messenger (the game's "buddy server", port 13505 of the online host): friends lists, friend requests, who is
//! online and what they do, messages, and game invites, between the personas logged in to this server's lobby.
//!
//! The game's buddy client (SPEED2.EXE BuddyApi, 0x74BC50-0x74F5F0) speaks the lobby's framing with upper-case
//! commands.  It logs in with AUTH and the LKEY its lobby login was given (lobby.rs `select_persona`): that key, from
//! the same browser, not anything typed, says which persona this connection is.  What it sends and what it is
//! answered here:
//!
//! - AUTH PROD VERS PRES USER LKEY -> AUTH TITL="EA MESSENGER" (unknown key: code "pass"; a second AUTH on a
//!   connection that is logged in: code "logn")
//! - EPGT LRSC ID -> EPGT ID ENAB=N ADDR=
//! - RGET LIST=B|I PRES PEND ID (the game's login asks for both, ID 1 and ID 2: a ROST's ID says which list its
//!   entry goes on) -> RGET ID SIZE=n, then n x ROST ID USER LIST (the friends, or the blocked; with PEND=Y also the
//!   friend requests that wait, ATTR=R one to this persona, ATTR=S one it sent), then PGET for each entry of
//!   B; `MAIL_WAIT_MS` later RECV for each message that came while the persona was away
//! - RADM USER PRES ID ("Send Friend Request", the only way the game makes a friend) -> RADM ID and the asked
//!   persona's presence (PGET); the request waits for its answer (`ASK_DAYS`), the asked persona told now if online
//!   (RNOT USER (the asker) CHNG=A ATTR=R, the asker's presence), else at its next login (ROST); a request back from
//!   the asked persona makes friends
//! - RRSP USER ANSW ID (the answer, 0x74DD00: Y accept, N decline, B block) -> RRSP ID; accepted: both are friends,
//!   each told (RNOT USER CHNG=A ATTR=0, PGET), the asker at its next login if away; declined or blocked: the request
//!   goes (to the asker: RNOT USER CHNG=D)
//! - RDEM USER (a friend taken off, the game's RDEL after it; or a request taken back) and RDEL LIST=B USER -> ack;
//!   the two are friends no more, on neither list, the other told (RNOT USER CHNG=D)
//! - RADD/RDEL LIST=I USER ID -> ack: blocking, kept with the persona's lists; RADD LIST=B (the game sends none) puts
//!   a friend on this persona's list only, the friend told (RNOT USER) and its presence to the adder
//! - PADD/PDEL USER -> ack, and that player's presence (the Messenger's "All" list: players searched for or met; the
//!   game's own, in memory only)
//! - PSET SHOW STAT PROD -> ack; when it changed, PGET USER SHOW STAT PROD TITL EXTR ATTR to every online player who
//!   has this persona as a friend (at most once a second, the latest)
//! - SEND USER BODY SUBJ TYPE -> SEND; RECV USER (the sender) BODY SUBJ TYPE TIME to the persona; to one that is away
//!   and has the sender as a friend it waits (`MAIL_DAYS`) for that persona's next login
//! - GINV/GRSP/GRVK USER ... -> ack; passed on to the persona (USER = the sender)
//! - USCH USER MAXR ID -> USCH ID SIZE=n, then n x USER ID USER (the Search: personas online or known from their
//!   races whose names start with the text)
//! - PING -> PING;  DISC -> closed
//!
//! Field names and the roster's bits from the game's own code (0x74C640 ROST, 0x74E2B0 RNOT, 0x74D420 the adds,
//! 0x74D660 the deletes); how its screens show each was checked only in part.
//!
//! Kept in <state>/online/buddies.json, so they hold over a restart: each persona's friends and blocked (names), the
//! friend requests it sent that wait for an answer, and the messages from friends that wait for it.  A persona
//! belongs to whoever plays under its name (there are no accounts), so whoever takes a name that is not online sees
//! and can change its lists and gets its waiting messages (docs/web/PLAYING.md; the blocked list was the session's
//! own until 2026-10-23, the Online audit's ONL-6, and is kept since for the players who block).  Invites are passed
//! on and forgotten (to a persona online now).  Limits: 50 friends, 50 blocked, `MAX_ASKED` requests waiting from one
//! persona, `MAX_MAIL` messages waiting for one (the oldest goes) and `ALL_MAIL` in all; `lists` lists, the least
//! recently seen going first, new ones `list_rate` an hour per client; messages `msg_rate` a second (burst 3 x), the
//! lists and the search `heavy_rate` a second (burst 4), over that: closed (the Online audit's ONL-1, ONL-3); chat
//! messages 2 a second (burst 5) and 500 characters; a blocked persona's messages, invites and requests never arrive.
//!
//! A buddy connection's state stays readable until the event that closed it is over, so the code still handling that
//! event goes on with it (a handler whose own answer closed the connection goes on with no persona, and fails where
//! it needs one: `null_persona`).

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::Arc;

use indexmap::IndexMap;
use nfsu2_util::json;
use serde_json::Value;

use crate::aries::{FieldMap, Fields, FrameReader, V, encode_frame};
use crate::core::Core;
use crate::jsx::{self, clean_text, lower, strip_at, valid_name};
use crate::lobby::{LBucket, four_printable};
use crate::omap::OrderedMap;
use crate::timers::{Action, TimerId};
use crate::title::Title;
use crate::vhost::{ConnId, Svc};

const MAX_LIST: usize = 50;
/// friend requests one persona has waiting, and the days one waits for its answer
const MAX_ASKED: usize = 20;
const ASK_DAYS: f64 = 30.0;
/// messages waiting for one persona, for all of them together, and the days one waits
const MAX_MAIL: usize = 20;
const ALL_MAIL: usize = 20_000;
const MAIL_DAYS: f64 = 7.0;
const DAY_MS: f64 = 86_400_000.0;
/// how long after the friends list the waiting messages go: the game files a message under the player's entry of its
/// Messenger, which a friend from the list gets only once every message the game had to read is handled (its RGET's
/// callback at the end of the update, 0x74E8E0: a RECV in the same batch as the ROST is dropped)
const MAIL_WAIT_MS: f64 = 3000.0;
/// the roster's request bits as ROST and RNOT write them: ATTR is a set of letters, each a bit of the game's entry
/// ('@' bit 0, 'A' bit 1 ... 'Z' bit 26, '0'-'3' bits 27-30: 0x742100, its table at 0x7D26B8), of which the game keeps
/// R (0x40000: a request from that player waits for this one's answer) and S (0x80000: one this player sent);
/// "0" has neither
const ASKED_ME: &str = "R";
const I_ASKED: &str = "S";

/// EA Messenger's limits (see the module's text): `lists` friends lists kept, `list_rate` new ones an hour per
/// client, `msg_rate` messages a second (burst 3 x), `heavy_rate` lists and searches a second (burst 4),
/// `presence_ms` the least time between two presences of one persona
#[derive(Clone, Copy, Debug)]
pub struct BuddyLimits {
    pub lists: f64,
    pub list_rate: f64,
    pub msg_rate: f64,
    pub heavy_rate: f64,
    pub presence_ms: f64,
}

impl Default for BuddyLimits {
    fn default() -> Self {
        BuddyLimits { lists: 5000.0, list_rate: 20.0, msg_rate: 10.0, heavy_rate: 1.0, presence_ms: 1000.0 }
    }
}

/// a message that waits for its persona: who sent it, its text, and when (ms since 1970)
pub struct Mail {
    pub from: String,
    pub body: String,
    pub subj: String,
    pub kind: String,
    pub t: f64,
}

/// a persona's lists: its name, the friends (lower case -> the name as given; `B` in buddies.json), the blocked (`I`),
/// the friend requests it sent that wait for an answer (lower case -> the name as given and when; `R`), the messages
/// that wait for it, the oldest first (`M`), and the month it was last seen
pub struct FriendsList {
    pub name: String,
    pub b: IndexMap<String, String>,
    pub i: IndexMap<String, String>,
    pub r: IndexMap<String, (String, f64)>,
    pub m: Vec<Mail>,
    pub seen: String,
}

impl FriendsList {
    fn new(name: &str, seen: String) -> FriendsList {
        FriendsList { name: name.to_string(), b: IndexMap::new(), i: IndexMap::new(), r: IndexMap::new(), m: Vec::new(), seen }
    }

    /// nothing in it to keep
    fn is_empty(&self) -> bool {
        self.b.is_empty() && self.i.is_empty() && self.r.is_empty() && self.m.is_empty()
    }
}

/// one EA Messenger connection
pub struct Buddy {
    pub conn: ConnId,
    vip: u32,
    group: Arc<str>,
    pub persona: Option<String>,
    /// the persona in lower case (the key of its friends list and of `online`), while there is one
    key: String,
    show: String,
    stat: String,
    prod: String,
    msgs_t: f64,
    msgs_v: f64,
    met: HashSet<String>,
    told: Option<f64>,
    presence_timer: Option<TimerId>,
    mail_timer: Option<TimerId>,
    rate: LBucket,
    heavy: LBucket,
    tries: u32,
    reader: FrameReader,
    login_timer: Option<TimerId>,
}

pub struct Buddies {
    pub l: BuddyLimits,
    dir: Option<PathBuf>,
    save_ms: f64,
    /// lower-case persona -> its friends list, least recently seen first
    pub lists: OrderedMap<String, FriendsList>,
    /// lower-case persona -> buddy connection
    online: OrderedMap<String, u64>,
    /// lower-case persona -> the lists (their owners, lower case) that have it as a friend, and the connections that
    /// met it: who hears of its presence (so a presence goes to the ones that hear, not through every online persona)
    fans: HashMap<String, HashSet<String>>,
    met_by: HashMap<String, HashSet<u64>>,
    /// lower-case persona -> the personas (lower case) whose friend requests wait for its answer (each kept in the
    /// asker's list, `r`)
    asked: HashMap<String, HashSet<String>>,
    /// the messages waiting, on all lists
    mail: usize,
    /// when requests and messages that waited too long were last let go
    pruned: f64,
    /// client group -> bucket: new friends lists
    made: HashMap<Arc<str>, LBucket>,
    timer: Option<TimerId>,
    dirty: bool,
    pub conns: HashMap<u64, Buddy>,
    graveyard: Vec<u64>,
    next: u64,
}

/// a time the file keeps (ms since 1970, whole)
fn when(v: &Value) -> Option<f64> {
    v.as_f64().filter(|t| (0.0..=1e15).contains(t)).map(f64::floor)
}

/// names of a list in buddies.json (`B`, `I`) onto a list's map: the later spelling of a name in the earlier one's
/// place, MAX_LIST at most
fn read_names(v: Option<&Value>, into: &mut IndexMap<String, String>) {
    if let Some(Value::Array(a)) = v {
        for n in a.iter().take(MAX_LIST) {
            if let Some(n) = n.as_str().filter(|n| valid_name(n)) {
                into.insert(lower(n), n.to_string());
            }
        }
    }
}

/// `,"<key>":[names]`
fn write_names<'a>(s: &mut String, key: &str, names: impl Iterator<Item = &'a String>) {
    s.push_str(",\"");
    s.push_str(key);
    s.push_str("\":[");
    for (i, n) in names.enumerate() {
        if i > 0 {
            s.push(',');
        }
        json::write_str(s, n);
    }
    s.push(']');
}

impl Buddies {
    pub fn new(dir: Option<PathBuf>, l: BuddyLimits) -> Buddies {
        let mut b = Buddies {
            l,
            dir,
            save_ms: 10_000.0,
            lists: OrderedMap::new(),
            online: OrderedMap::new(),
            fans: HashMap::new(),
            met_by: HashMap::new(),
            asked: HashMap::new(),
            mail: 0,
            pruned: f64::NEG_INFINITY,
            made: HashMap::new(),
            timer: None,
            dirty: false,
            conns: HashMap::new(),
            graveyard: Vec::new(),
            next: 0,
        };
        if b.dir.is_some() {
            b.load();
        }
        b
    }

    fn file(&self) -> Option<PathBuf> {
        self.dir.as_ref().map(|d| d.join("buddies.json"))
    }

    fn load(&mut self) {
        let Some(file) = self.file() else { return };
        let Ok(bytes) = std::fs::read(&file) else { return };
        let Ok(j) = crate::jsonparse::parse(&String::from_utf8_lossy(&bytes)) else { return };
        // (a file of version 2 or older keeps only the friends: one of before the Online audit also has the blocked
        // lists of then, I, which are not read)
        let v3 = j.get("v").and_then(Value::as_f64).is_some_and(|v| v >= 3.0);
        let mut rows: Vec<(&serde_json::Map<String, Value>, String, String)> = match j.get("personas") {
            Some(Value::Array(a)) => a
                .iter()
                .filter_map(|r| {
                    let o = r.as_object()?;
                    let name = o.get("name")?.as_str()?;
                    if !valid_name(name) {
                        return None;
                    }
                    // (a `seen` that is not a string counts as none: see stats.rs `clean`)
                    let seen = o.get("seen").and_then(Value::as_str).filter(|s| jsx::is_month(s)).unwrap_or("1970-01");
                    Some((o, name.to_string(), seen.to_string()))
                })
                .collect(),
            _ => Vec::new(),
        };
        rows.sort_by(|a, b| a.2.cmp(&b.2));
        let from = rows.len().saturating_sub(self.l.lists as usize);
        for (r, name, seen) in rows.drain(from..) {
            let mut l = FriendsList::new(&name, seen);
            read_names(r.get("B"), &mut l.b);
            if v3 {
                read_names(r.get("I"), &mut l.i);
                if let Some(Value::Array(a)) = r.get("R") {
                    for e in a.iter().take(MAX_ASKED) {
                        if let Some([n, t]) = e.as_array().map(Vec::as_slice)
                            && let Some(n) = n.as_str().filter(|n| valid_name(n))
                            && let Some(t) = when(t)
                        {
                            l.r.insert(lower(n), (n.to_string(), t));
                        }
                    }
                }
                if let Some(Value::Array(a)) = r.get("M") {
                    // (the newest MAX_MAIL)
                    for e in a.iter().skip(a.len().saturating_sub(MAX_MAIL)) {
                        let Some(o) = e.as_object() else { continue };
                        let (Some(from), Some(t)) =
                            (o.get("from").and_then(Value::as_str).filter(|n| valid_name(n)), o.get("t").and_then(when))
                        else {
                            continue;
                        };
                        let text = |k: &str, n: usize| o.get(k).and_then(Value::as_str).map_or(String::new(), |s| clean_text(s, n));
                        let kind = o.get("type").and_then(Value::as_str).map_or("C", |s| jsx::slice16(s, 4)).to_string();
                        l.m.push(Mail { from: from.to_string(), body: text("body", 500), subj: text("subj", 64), kind, t });
                    }
                }
            }
            if !l.is_empty() {
                self.lists.insert(lower(&name), l);
            }
        }
        // (a name twice in the file: the later list, in the earlier one's place)
        for (owner, l) in self.lists.iter() {
            for k in l.b.keys() {
                self.fans.entry(k.clone()).or_default().insert(owner.clone());
            }
            for k in l.r.keys() {
                self.asked.entry(k.clone()).or_default().insert(owner.clone());
            }
            self.mail += l.m.len();
        }
    }

    /// a friend on owner's list (false: it has none)
    fn add_friend(&mut self, owner: &str, k: String, name: String) -> bool {
        let Some(l) = self.lists.get_mut(owner) else { return false };
        l.b.insert(k.clone(), name);
        self.fans.entry(k).or_default().insert(owner.to_string());
        true
    }

    /// a friend off owner's list (false: it was not on it)
    fn remove_friend(&mut self, owner: &str, k: &str) -> bool {
        if !self.lists.get_mut(owner).is_some_and(|l| l.b.shift_remove(k).is_some()) {
            return false;
        }
        self.unfan(k, owner);
        true
    }

    /// owner (lower case) blocks k
    pub fn blocks(&self, owner: &str, k: &str) -> bool {
        self.lists.get(owner).is_some_and(|l| l.i.contains_key(k))
    }

    /// from's friend request to `to` (both lower case) while it waits: the asker's name
    fn request(&self, from: &str, to: &str, now: f64) -> Option<&str> {
        let l = self.lists.get(from)?;
        let (_, t) = l.r.get(to)?;
        (now - t < ASK_DAYS * DAY_MS).then_some(l.name.as_str())
    }

    /// from's request to `to` (lower case `to`, its name as given), on from's list (which is there)
    fn ask(&mut self, from: &str, to: String, name: String, t: f64) {
        if let Some(l) = self.lists.get_mut(from) {
            l.r.insert(to.clone(), (name, t));
            self.asked.entry(to).or_default().insert(from.to_string());
        }
    }

    /// from's request to `to` goes (false: there was none)
    fn unask(&mut self, from: &str, to: &str) -> bool {
        if !self.lists.get_mut(from).is_some_and(|l| l.r.shift_remove(to).is_some()) {
            return false;
        }
        self.unindex(to, from);
        true
    }

    fn unindex(&mut self, to: &str, from: &str) {
        if let Some(s) = self.asked.get_mut(to) {
            s.remove(from);
            if s.is_empty() {
                self.asked.remove(to);
            }
        }
    }

    /// a list goes (the store is full)
    fn drop_list(&mut self, owner: &str) {
        if let Some(l) = self.lists.remove(owner) {
            for k in l.b.keys() {
                self.unfan(k, owner);
            }
            for k in l.r.keys() {
                self.unindex(k, owner);
            }
            self.mail -= l.m.len();
        }
    }

    fn unfan(&mut self, k: &str, owner: &str) {
        if let Some(s) = self.fans.get_mut(k) {
            s.remove(owner);
            if s.is_empty() {
                self.fans.remove(k);
            }
        }
    }

    /// the requests and messages that waited too long go (false: none did)
    fn prune(&mut self, now: f64) -> bool {
        let mut gone: Vec<(String, String)> = Vec::new();
        let mut mail = 0;
        self.lists.for_each_mut(|owner, l| {
            l.r.retain(|k, (_, t)| {
                let keep = now - *t < ASK_DAYS * DAY_MS;
                if !keep {
                    gone.push((k.clone(), owner.clone()));
                }
                keep
            });
            let n = l.m.len();
            l.m.retain(|m| now - m.t < MAIL_DAYS * DAY_MS);
            mail += n - l.m.len();
        });
        for (to, from) in &gone {
            self.unindex(to, from);
        }
        self.mail -= mail;
        !gone.is_empty() || mail > 0
    }

    /// buddies.json's text: version 3, the lists that have anything in them (`B` always, `I`, `R` and `M` when not
    /// empty)
    pub fn serialize(&self) -> String {
        let mut s = String::from(
            "{\"v\":3,\"note\":\"NFSU2 online Messenger lists (persona names; friend requests and messages waiting for their persona)\",\"personas\":[",
        );
        let mut first = true;
        for l in self.lists.values().filter(|l| !l.is_empty()) {
            if !first {
                s.push(',');
            }
            first = false;
            s.push_str("{\"name\":");
            json::write_str(&mut s, &l.name);
            s.push_str(",\"seen\":");
            json::write_str(&mut s, &l.seen);
            write_names(&mut s, "B", l.b.values());
            if !l.i.is_empty() {
                write_names(&mut s, "I", l.i.values());
            }
            if !l.r.is_empty() {
                s.push_str(",\"R\":[");
                for (i, (n, t)) in l.r.values().enumerate() {
                    if i > 0 {
                        s.push(',');
                    }
                    s.push('[');
                    json::write_str(&mut s, n);
                    s.push_str(&format!(",{}]", *t as i64));
                }
                s.push(']');
            }
            if !l.m.is_empty() {
                s.push_str(",\"M\":[");
                for (i, m) in l.m.iter().enumerate() {
                    if i > 0 {
                        s.push(',');
                    }
                    s.push_str("{\"from\":");
                    json::write_str(&mut s, &m.from);
                    s.push_str(&format!(",\"t\":{},\"body\":", m.t as i64));
                    json::write_str(&mut s, &m.body);
                    s.push_str(",\"subj\":");
                    json::write_str(&mut s, &m.subj);
                    s.push_str(",\"type\":");
                    json::write_str(&mut s, &m.kind);
                    s.push('}');
                }
                s.push(']');
            }
            s.push('}');
        }
        s.push_str("]}");
        s
    }

    /// a persona's friends list, or None: none is made by asking
    pub fn list(&self, name: &str) -> Option<&FriendsList> {
        self.lists.get(&lower(name))
    }

    pub fn purge(&mut self) {
        for id in std::mem::take(&mut self.graveyard) {
            let Some(x) = self.conns.remove(&id) else { continue };
            for k in x.met {
                if let Some(s) = self.met_by.get_mut(&k) {
                    s.remove(&id);
                    if s.is_empty() {
                        self.met_by.remove(&k);
                    }
                }
            }
        }
    }
}

/// a persona's presence as its friends see it (TITL and EXTR: the title's)
fn presence(t: &Title, b: &Buddy) -> Fields {
    fields![
        ("USER", &b.persona),
        ("SHOW", &b.show),
        ("STAT", &b.stat),
        ("PROD", &b.prod),
        ("TITL", t.messenger_titl),
        ("EXTR", t.messenger_extr),
        ("ATTR", "D")
    ]
}

/// the presence of a persona that is not online
fn offline(t: &Title, name: &str) -> Fields {
    fields![
        ("USER", name),
        ("SHOW", "DISC"),
        ("STAT", ""),
        ("PROD", ""),
        ("TITL", t.messenger_titl),
        ("EXTR", t.messenger_extr),
        ("ATTR", "D")
    ]
}

/// the error of a handler that needs the connection's persona once it is gone (the console line the Node server
/// wrote for it)
fn null_persona() -> String {
    "TypeError: Cannot read properties of null (reading 'toLowerCase')".into()
}

impl Core {
    /// the connection's words to the game (an error: a field name that is not one)
    fn bsend(&mut self, b: u64, kind: &str, fields: &[(std::borrow::Cow<'static, str>, V)], code: &str) -> Result<(), String> {
        let f = encode_frame(kind, code, fields)?;
        if let Some(conn) = self.buddies.conns.get(&b).map(|x| x.conn) {
            self.conn_write(conn, &f);
        }
        Ok(())
    }

    /// a message encoded once, to one more connection
    fn bsend_frame(&mut self, b: u64, frame: &[u8]) {
        if let Some(conn) = self.buddies.conns.get(&b).map(|x| x.conn) {
            self.conn_write(conn, frame);
        }
    }

    fn bnow(&self) -> f64 {
        (self.clocks.now)()
    }

    /// the presence of persona `name` (lower case `k`): its connection's, or offline
    fn presence_of(&self, k: &str, name: &str) -> Fields {
        match self.buddies.online.get(k).and_then(|o| self.buddies.conns.get(o)) {
            Some(o) => presence(self.title, o),
            None => offline(self.title, name),
        }
    }

    pub fn buddy_connect(&mut self, c: ConnId) {
        let Some((vip, _, group, _)) = self.conn_ids(c) else { return };
        let now = self.bnow();
        let l = self.buddies.l;
        self.buddies.next += 1;
        let id = self.buddies.next;
        let t = self.set_timeout(60_000.0, Action::BuddyLogin(id));
        self.buddies.conns.insert(
            id,
            Buddy {
                conn: c,
                vip,
                group,
                persona: None,
                key: String::new(),
                show: "CHAT".into(),
                stat: String::new(),
                prod: String::new(),
                msgs_t: now,
                msgs_v: 5.0,
                met: HashSet::new(),
                told: None,
                presence_timer: None,
                mail_timer: None,
                rate: LBucket::new(l.msg_rate, 3.0 * l.msg_rate, now),
                heavy: LBucket::new(l.heavy_rate, 4.0, now),
                tries: 0,
                reader: FrameReader::default(),
                login_timer: Some(t),
            },
        );
        self.set_svc(c, Svc::Buddy(id));
    }

    /// the 60 s for a login are over
    pub fn buddy_login_idle(&mut self, b: u64) {
        if let Some(conn) = self.buddies.conns.get(&b).map(|x| x.conn) {
            self.conn_destroy(conn, "buddy: no login");
        }
    }

    /// the connection closed (reset, ended or gone with its browser): its login timer ends, it is gone
    pub fn buddy_closed(&mut self, b: u64) {
        if let Some(t) = self.buddies.conns.get_mut(&b).and_then(|x| x.login_timer.take()) {
            self.timers.clear(t);
        }
        self.buddy_gone(b);
        self.buddies.graveyard.push(b);
    }

    pub fn buddy_data(&mut self, b: u64, buf: &[u8]) {
        let Some(x) = self.buddies.conns.get_mut(&b) else { return };
        let msgs = x.reader.push(buf);
        let conn = x.conn;
        for m in msgs {
            if !self.conn_open(conn) {
                return;
            }
            let now = self.bnow();
            if !self.buddies.conns.get_mut(&b).is_some_and(|x| x.rate.take(now, 1.0)) {
                return self.conn_destroy(conn, "buddy: too many messages");
            }
            if !four_printable(&m.kind) {
                return self.conn_destroy(conn, "buddy: not the lobby protocol");
            }
            let f = FieldMap::parse(&m.body);
            if let Err(e) = self.buddy_message(b, &m.kind, &f) {
                // the error as "<name>: <message>" (a TypeError names itself, the others are an Error)
                let e = if e.starts_with("TypeError") { e } else { format!("Error: {e}") };
                self.console_error(&format!("[online] buddy \"{}\" failed: {e}", m.kind));
                return self.conn_destroy(conn, "server error");
            }
        }
        if self.buddies.conns.get(&b).is_some_and(|x| x.reader.error.is_some()) {
            self.conn_destroy(conn, "buddy: not the lobby protocol");
        }
    }

    /// the connection's persona goes offline: its held-back presence and waiting messages' timer dropped, off
    /// `online`, its friends told
    pub fn buddy_gone(&mut self, b: u64) {
        let Some(x) = self.buddies.conns.get_mut(&b) else { return };
        for t in [x.presence_timer.take(), x.mail_timer.take()].into_iter().flatten() {
            self.timers.clear(t);
        }
        if x.persona.is_none() {
            return;
        }
        // every name that points at this connection (there is one: a second AUTH is refused)
        let keys: Vec<String> = self.buddies.online.iter().filter(|(_, o)| **o == b).map(|(k, _)| k.clone()).collect();
        for k in &keys {
            self.buddies.online.remove(k);
        }
        if !keys.is_empty() {
            let _ = self.buddy_announce(b, true);
        }
        if let Some(x) = self.buddies.conns.get_mut(&b) {
            x.persona = None;
            x.key.clear();
        }
    }

    fn touch_lists(&mut self) {
        self.buddies.dirty = true;
        if self.buddies.dir.is_none() || self.buddies.timer.is_some() {
            return;
        }
        let ms = self.buddies.save_ms;
        self.buddies.timer = Some(self.set_timeout(ms, Action::BuddySave));
    }

    /// the friends lists' save timer fired
    pub fn buddy_save_timer(&mut self) {
        self.buddies.timer = None;
        self.buddy_save();
    }

    pub fn buddy_save(&mut self) {
        let Some(dir) = self.buddies.dir.clone() else { return };
        // (what waited too long is not written)
        let now = self.bnow();
        if self.buddies.prune(now) {
            self.buddies.dirty = true;
        }
        if !self.buddies.dirty {
            return;
        }
        let body = self.buddies.serialize();
        match crate::files::write_atomic(&dir, &dir.join("buddies.json"), body.as_bytes()) {
            Ok(()) => self.buddies.dirty = false,
            Err(e) => (self.log)(&format!("[online] the friends lists could not be written: {e}"), None),
        }
    }

    pub fn buddy_stop(&mut self) {
        if let Some(t) = self.buddies.timer.take() {
            self.timers.clear(t);
        }
        self.buddy_save();
    }

    /// a persona's friends list, made if needed (on behalf of client `group`): false when that client made its share
    /// of new lists this hour; the least recently seen list goes when there are too many
    fn make_list(&mut self, name: &str, group: &Arc<str>) -> bool {
        let k = lower(name);
        if self.buddies.lists.contains_key(&k) {
            return true;
        }
        let now = self.bnow();
        let l = self.buddies.l;
        if !self.buddies.made.contains_key(group) {
            if self.buddies.made.len() >= 10_000 {
                self.buddies.made.retain(|_, x| !(x.take(now, 0.0) && x.v >= x.cap));
            }
            self.buddies.made.insert(group.clone(), LBucket::new(l.list_rate / 3600.0, l.list_rate, now));
        }
        if !self.buddies.made.get_mut(group).expect("just made").take(now, 1.0) {
            return false;
        }
        while self.buddies.lists.len() as f64 >= l.lists {
            let Some(old) = self.buddies.lists.first_key().cloned() else { break };
            self.buddies.drop_list(&old);
            self.buddies.dirty = true;
        }
        self.buddies.lists.insert(k, FriendsList::new(name, jsx::month_of(now)));
        true
    }

    /// a persona was seen: its list moves to the most recently seen end
    fn seen_list(&mut self, name: &str) {
        let k = lower(name);
        if !self.buddies.lists.contains_key(&k) {
            return;
        }
        self.buddies.lists.move_to_end(&k);
        let m = jsx::month_of(self.bnow());
        let l = self.buddies.lists.get_mut(&k).expect("there");
        if l.seen != m {
            l.seen = m;
            self.touch_lists();
        }
    }

    /// every online persona that has b as a friend gets its presence now
    fn buddy_announce(&mut self, b: u64, gone: bool) -> Result<(), String> {
        let x = &self.buddies.conns[&b];
        let Some(p) = x.persona.clone() else { return Err(null_persona()) };
        let me = lower(&p);
        let what = if gone { offline(self.title, &p) } else { presence(self.title, x) };
        let now = self.bnow();
        self.buddies.conns.get_mut(&b).expect("held").told = Some(now);
        let frame = encode_frame("PGET", "", &what)?;
        // the online personas that have b as a friend or met it, in the order of `online` (the order they logged in):
        // what is on the lists and who met whom do not change while this goes out, and a connection is in `online`
        // exactly while it has a persona (one that went during this loop is passed over)
        let bs = &self.buddies;
        let mut hear: Vec<(u64, u64)> = Vec::new();
        for owner in bs.fans.get(&me).into_iter().flatten() {
            if let Some((place, o)) = bs.online.get_placed(owner) {
                hear.push((place, *o));
            }
        }
        for o in bs.met_by.get(&me).into_iter().flatten() {
            if let Some(ox) = bs.conns.get(o)
                && ox.persona.is_some()
                && let Some((place, x)) = bs.online.get_placed(&ox.key)
                && x == o
            {
                hear.push((place, *o));
            }
        }
        hear.sort_unstable();
        hear.dedup();
        for (_, o) in hear {
            if o != b && self.buddies.conns.get(&o).is_some_and(|x| x.persona.is_some()) {
                self.bsend_frame(o, &frame);
            }
        }
        Ok(())
    }

    /// b's presence changed: out now, or the latest of it once presence_ms has passed since the last
    fn presence_changed(&mut self, b: u64) -> Result<(), String> {
        let x = &self.buddies.conns[&b];
        if x.presence_timer.is_some() {
            return Ok(());
        }
        let wait = x.told.unwrap_or(f64::NEG_INFINITY) + self.buddies.l.presence_ms - self.bnow();
        if wait <= 0.0 {
            return self.buddy_announce(b, false);
        }
        let t = self.set_timeout(wait, Action::Presence(b));
        self.buddies.conns.get_mut(&b).expect("held").presence_timer = Some(t);
        Ok(())
    }

    /// a held-back presence goes out
    pub fn presence_timer(&mut self, b: u64) {
        let Some(x) = self.buddies.conns.get_mut(&b) else { return };
        x.presence_timer = None;
        if x.persona.is_some() {
            let _ = self.buddy_announce(b, false);
        }
    }

    /// the messages that waited for the connection's persona go to it
    pub fn buddy_mail_timer(&mut self, b: u64) {
        let Some(x) = self.buddies.conns.get_mut(&b) else { return };
        x.mail_timer = None;
        let (conn, me) = (x.conn, x.persona.as_ref().map(|_| x.key.clone()));
        if let Some(me) = me
            && self.conn_open(conn)
        {
            let _ = self.deliver_mail(b, &me);
        }
    }

    /// b and the persona who become friends (each on the other's list), both told (RNOT ATTR=0: no request pending
    /// any more) with each other's presence
    fn befriend(&mut self, b: u64, who: &str, how: &str) -> Result<(), String> {
        let x = &self.buddies.conns[&b];
        let Some(bp) = x.persona.clone() else { return Err(null_persona()) };
        let group = x.group.clone();
        let (k, me) = (lower(who), lower(&bp));
        if self.buddies.unask(&k, &me) | self.buddies.unask(&me, &k) {
            self.touch_lists();
        }
        let full_without = |l: Option<&FriendsList>, n: &str| l.is_some_and(|l| l.b.len() >= MAX_LIST && !l.b.contains_key(n));
        if full_without(self.buddies.list(&bp), &k) || full_without(self.buddies.list(who), &me) {
            return Ok(());
        }
        let mine = self.make_list(&bp, &group);
        let them = mine && self.make_list(who, &group);
        if !mine || !them {
            return Ok(());
        }
        // (a list the second one pushed out of the store takes its friend nowhere: `add_friend` finds no list)
        for (l, n) in [(&me, who), (&k, bp.as_str())] {
            self.buddies.add_friend(l, lower(n), n.to_string());
        }
        self.touch_lists();
        (self.log)(&format!("[online] \"{bp}\" and \"{who}\" are friends ({how})"), Some(&group));
        let o = self.buddies.online.get(&k).copied();
        self.bsend(b, "RNOT", &fields![("USER", who), ("CHNG", "A"), ("ATTR", 0)], "")?;
        let p = self.presence_of(&k, who);
        self.bsend(b, "PGET", &p, "")?;
        if let Some(o) = o {
            let bp2 = self.buddies.conns.get(&b).and_then(|x| x.persona.clone());
            self.bsend(o, "RNOT", &fields![("USER", bp2), ("CHNG", "A"), ("ATTR", 0)], "")?;
            let p = presence(self.title, &self.buddies.conns[&b]);
            self.bsend(o, "PGET", &p, "")?;
        }
        Ok(())
    }

    /// persona `me_name` and `who` are friends no more (on neither list), and me's request to who is taken back: who
    /// told (RNOT CHNG=D) when that changed anything
    fn unfriend(&mut self, me_name: &str, who: &str) -> Result<(), String> {
        let (me, k) = (lower(me_name), lower(who));
        let changed = self.buddies.remove_friend(&me, &k) | self.buddies.remove_friend(&k, &me) | self.buddies.unask(&me, &k);
        if !changed {
            return Ok(());
        }
        self.touch_lists();
        match self.buddies.online.get(&k).copied() {
            Some(o) => self.bsend(o, "RNOT", &fields![("USER", me_name), ("CHNG", "D")], ""),
            None => Ok(()),
        }
    }

    /// `who` (lower case k) on me's blocked list, its request to me gone; false: no room
    fn block(&mut self, me_name: &str, group: &Arc<str>, who: String) -> bool {
        let (me, k) = (lower(me_name), lower(&who));
        if self.buddies.list(me_name).is_some_and(|l| l.i.len() >= MAX_LIST && !l.i.contains_key(&k)) || !self.make_list(me_name, group) {
            return false;
        }
        self.buddies.lists.get_mut(&me).expect("made").i.insert(k.clone(), who);
        self.buddies.unask(&k, &me);
        self.touch_lists();
        true
    }

    /// a message to persona k (lower case) that is away waits for it when k has the sender as a friend and does not
    /// block it, while there is room (the oldest of k's goes for it)
    fn keep_mail(&mut self, k: &str, from: &str, f: &FieldMap, t: f64) {
        let me = lower(from);
        if self.buddies.mail >= ALL_MAIL || self.buddies.blocks(k, &me) {
            return;
        }
        let Some(l) = self.buddies.lists.get_mut(k).filter(|l| l.b.contains_key(&me)) else { return };
        l.m.push(Mail {
            from: from.to_string(),
            body: clean_text(f.str("BODY"), 500),
            subj: clean_text(f.str("SUBJ"), 64),
            kind: jsx::slice16(f.get("TYPE").unwrap_or("C"), 4).to_string(),
            t: t.floor(),
        });
        self.buddies.mail += 1;
        if l.m.len() > MAX_MAIL {
            l.m.remove(0);
            self.buddies.mail -= 1;
        }
        self.touch_lists();
    }

    /// the messages that waited for persona `me` (lower case), to connection b, and gone from the store (a sender
    /// blocked since is left out)
    fn deliver_mail(&mut self, b: u64, me: &str) -> Result<(), String> {
        let Some(l) = self.buddies.lists.get_mut(me).filter(|l| !l.m.is_empty()) else { return Ok(()) };
        let mail = std::mem::take(&mut l.m);
        self.buddies.mail -= mail.len();
        self.touch_lists();
        let now = self.bnow();
        for m in mail {
            if now - m.t >= MAIL_DAYS * DAY_MS || self.buddies.blocks(me, &lower(&m.from)) {
                continue;
            }
            let rec = fields![("USER", m.from), ("BODY", m.body), ("SUBJ", m.subj), ("TYPE", m.kind), ("TIME", (m.t / 1000.0).floor())];
            self.bsend(b, "RECV", &rec, "")?;
        }
        Ok(())
    }

    /// the AUTH tried again
    pub fn buddy_retry(&mut self, b: u64, f: FieldMap) {
        let Some(x) = self.buddies.conns.get(&b) else { return };
        if !self.conn_open(x.conn) || x.persona.is_some() {
            return;
        }
        let _ = self.buddy_message(b, "AUTH", &f);
    }

    fn buddy_message(&mut self, b: u64, kind: &str, f: &FieldMap) -> Result<(), String> {
        let with_id: Fields = match f.get("ID") {
            Some(id) => fields![("ID", id)],
            None => vec![],
        };
        let wid = |mut extra: Fields| {
            let mut v = with_id.clone();
            v.append(&mut extra);
            v
        };
        let x = &self.buddies.conns[&b];
        if kind == "AUTH" {
            if x.persona.is_some() {
                return self.bsend(b, "AUTH", &fields![("TITL", "EA MESSENGER")], "logn");
            }
            // the lobby's key for this browser's persona: the only thing that says who this is
            let key = f.str("LKEY").to_string();
            let vip = x.vip;
            // the first of this browser's live sessions with this key (a session of another browser never counts)
            let s = if key.encode_utf16().count() >= 24 {
                self.lobby.of_vip(vip).iter().copied().find(|id| {
                    let y = self.lobby.s(*id);
                    y.vip == vip && y.lkey.as_ref().is_some_and(|l| !l.is_empty() && l.starts_with(&key))
                })
            } else {
                None
            };
            let Some(s) = s else {
                let x = self.buddies.conns.get_mut(&b).expect("held");
                x.tries += 1;
                // the lobby login may finish a moment later
                if x.tries <= 6 {
                    self.set_timeout(500.0, Action::BuddyRetry(b, f.clone()));
                    return Ok(());
                }
                return self.bsend(b, "AUTH", &fields![("TITL", "EA MESSENGER")], "pass");
            };
            if let Some(t) = self.buddies.conns.get_mut(&b).and_then(|x| x.login_timer.take()) {
                self.timers.clear(t);
            }
            let persona = self.lobby.s(s).persona.clone().unwrap_or_default();
            let k = lower(&persona);
            if let Some(old) = self.buddies.online.get(&k).copied().filter(|o| *o != b)
                && let Some(conn) = self.buddies.conns.get(&old).map(|x| x.conn)
            {
                self.conn_destroy(conn, "logged in again");
            }
            if let Some(x) = self.buddies.conns.get_mut(&b) {
                x.persona = Some(persona.clone());
                x.key = k.clone();
            }
            self.buddies.online.insert(k, b);
            self.seen_list(&persona);
            // (requests and messages that waited too long go, at most once an hour)
            let now = self.bnow();
            if now - self.buddies.pruned >= 3_600_000.0 {
                self.buddies.pruned = now;
                if self.buddies.prune(now) {
                    self.touch_lists();
                }
            }
            return self.bsend(b, "AUTH", &fields![("TITL", "EA MESSENGER")], "");
        }
        if kind == "PING" {
            return self.bsend(b, "PING", &[], "");
        }
        if kind == "DISC" {
            self.buddy_gone(b);
            let conn = self.buddies.conns[&b].conn;
            self.conn_end(conn);
            return Ok(());
        }
        let short = jsx::slice16(kind, 4).to_string();
        let Some(bp) = self.buddies.conns[&b].persona.clone() else { return self.bsend(b, &short, &[], "auth") };
        let me = lower(&bp);
        let group = self.buddies.conns[&b].group.clone();
        let list_of = |f: &FieldMap| if f.str("LIST").to_uppercase() == "I" { "I" } else { "B" };
        let list_field = |f: &FieldMap| if f.has("LIST") { list_of(f) } else { "B" };
        match kind {
            "EPGT" => self.bsend(b, "EPGT", &wid(fields![("ENAB", "N"), ("ADDR", "")]), ""),
            "RGET" => {
                let now = self.bnow();
                if !self.buddies.conns.get_mut(&b).expect("held").heavy.take(now, 1.0) {
                    return self.bsend(b, "RGET", &with_id, "full");
                }
                let which = list_field(f);
                // the entries: name, ATTR (a request's letter, none for a friend or a blocked one)
                let mut rows: Vec<(String, &'static str)> = Vec::new();
                let bs = &self.buddies;
                let l = bs.lists.get(&me);
                if which == "I" {
                    rows.extend(l.into_iter().flat_map(|l| l.i.values()).map(|n| (n.clone(), "")));
                } else {
                    rows.extend(l.into_iter().flat_map(|l| l.b.values()).map(|n| (n.clone(), "")));
                    if f.str("PEND").eq_ignore_ascii_case("Y") {
                        let friend_or_asked = |k: &str| l.is_some_and(|l| l.b.contains_key(k) || l.r.contains_key(k));
                        for (k, (n, t)) in l.into_iter().flat_map(|l| l.r.iter()) {
                            if now - t < ASK_DAYS * DAY_MS && !l.is_some_and(|l| l.b.contains_key(k)) {
                                rows.push((n.clone(), I_ASKED));
                            }
                        }
                        let mut askers: Vec<&String> = bs.asked.get(&me).into_iter().flatten().collect();
                        askers.sort();
                        for k in askers {
                            if let Some(n) = bs.request(k, &me, now)
                                && !bs.blocks(&me, k)
                                && !friend_or_asked(k)
                            {
                                rows.push((n.to_string(), ASKED_ME));
                            }
                        }
                    }
                }
                self.bsend(b, "RGET", &wid(fields![("SIZE", rows.len())]), "")?;
                for (n, a) in &rows {
                    let mut r = wid(fields![("USER", n), ("LIST", which)]);
                    if !a.is_empty() {
                        r.extend(fields![("ATTR", *a)]);
                    }
                    self.bsend(b, "ROST", &r, "")?;
                }
                if which == "B" {
                    for (n, _) in &rows {
                        let p = self.presence_of(&lower(n), n);
                        self.bsend(b, "PGET", &p, "")?;
                    }
                    // the messages that waited, a moment later (`MAIL_WAIT_MS`)
                    let waiting = self.buddies.lists.get(&me).is_some_and(|l| !l.m.is_empty());
                    if waiting && self.buddies.conns[&b].mail_timer.is_none() {
                        let t = self.set_timeout(MAIL_WAIT_MS, Action::BuddyMail(b));
                        self.buddies.conns.get_mut(&b).expect("held").mail_timer = Some(t);
                    }
                }
                Ok(())
            }
            "RADD" | "RDEL" | "RDEM" => {
                let which = if kind == "RDEM" { "B" } else { list_field(f) };
                let who = strip_at(f.str("USER")).to_string();
                if !valid_name(&who) || lower(&who) == me {
                    return self.bsend(b, kind, &with_id, "user");
                }
                let k = lower(&who);
                if which == "I" {
                    if kind == "RADD" {
                        if !self.block(&bp, &group, who) {
                            return self.bsend(b, kind, &with_id, "full");
                        }
                    } else if self.buddies.lists.get_mut(&me).is_some_and(|l| l.i.shift_remove(&k).is_some()) {
                        self.touch_lists();
                    }
                    return self.bsend(b, kind, &with_id, "");
                }
                if kind != "RADD" {
                    // a friend taken off, or the request taken back: on neither side any more
                    self.bsend(b, kind, &with_id, "")?;
                    return self.unfriend(&bp, &who);
                }
                let cur = self.buddies.list(&bp);
                if cur.is_some_and(|l| l.b.len() >= MAX_LIST && !l.b.contains_key(&k)) {
                    return self.bsend(b, kind, &with_id, "full");
                }
                if cur.is_none() && !self.make_list(&bp, &group) {
                    return self.bsend(b, kind, &with_id, "full");
                }
                self.buddies.add_friend(&me, k.clone(), who.clone());
                self.touch_lists();
                self.bsend(b, kind, &with_id, "")?;
                let o = self.buddies.online.get(&k).copied();
                let p = self.presence_of(&k, &who);
                self.bsend(b, "PGET", &p, "")?;
                if let Some(o) = o.filter(|_| !self.buddies.blocks(&k, &me)) {
                    let bp2 = self.buddies.conns.get(&b).and_then(|x| x.persona.clone());
                    self.bsend(o, "RNOT", &fields![("USER", bp2), ("LIST", "B")], "")?;
                }
                Ok(())
            }
            "RADM" => {
                // a friend request (Send Friend Request): it waits for the persona's answer, RRSP
                let who = strip_at(f.str("USER")).to_string();
                let k = lower(&who);
                if !valid_name(&who) || k == me {
                    return self.bsend(b, kind, &with_id, "user");
                }
                let now = self.bnow();
                let (friends, full, asked_full) = match self.buddies.list(&bp) {
                    Some(l) => (l.b.contains_key(&k), l.b.len() >= MAX_LIST, l.r.len() >= MAX_ASKED && !l.r.contains_key(&k)),
                    None => (false, false, false),
                };
                if friends {
                    return self.bsend(b, kind, &with_id, "");
                }
                if full {
                    return self.bsend(b, kind, &with_id, "full");
                }
                // two requests that cross (the asked one asks back) make friends
                if self.buddies.request(&k, &me, now).is_some() {
                    self.bsend(b, kind, &with_id, "")?;
                    return self.befriend(b, &who, "asked back");
                }
                if asked_full || !self.make_list(&bp, &group) {
                    return self.bsend(b, kind, &with_id, "full");
                }
                self.bsend(b, kind, &with_id, "")?;
                // the asked persona's presence for the asker's new entry (PRES=Y; else the game shows it offline)
                let p = self.presence_of(&k, &who);
                self.bsend(b, "PGET", &p, "")?;
                // (a persona that blocks the asker never hears of it)
                if self.buddies.blocks(&k, &me) {
                    return Ok(());
                }
                self.buddies.ask(&me, k.clone(), who.clone(), now.floor());
                self.touch_lists();
                (self.log)(&format!("[online] \"{bp}\" asked \"{who}\" to be friends"), Some(&group));
                let Some(o) = self.buddies.online.get(&k).copied() else { return Ok(()) };
                // to the asked one: the asker, as a request that waits for its answer (R: the player's menu then
                // offers to answer it, Accept, Decline or Block, 0x4D5190/0x4A1B50), then the asker's presence for
                // that entry
                self.bsend(o, "RNOT", &fields![("USER", bp.as_str()), ("CHNG", "A"), ("ATTR", ASKED_ME)], "")?;
                let p = presence(self.title, &self.buddies.conns[&b]);
                self.bsend(o, "PGET", &p, "")
            }
            "RRSP" => {
                // the answer to a friend request: Y accept, N decline, B block
                let who = strip_at(f.str("USER")).to_string();
                let k = lower(&who);
                let now = self.bnow();
                let asker = self.buddies.request(&k, &me, now).map(str::to_string);
                self.bsend(b, kind, &with_id, "")?;
                // (one that waited too long goes too)
                if self.buddies.unask(&k, &me) {
                    self.touch_lists();
                }
                let Some(asker) = asker else { return Ok(()) };
                let answer = f.str("ANSW");
                let a = answer.chars().next().map(|c| c.to_ascii_uppercase());
                if a == Some('Y') {
                    return self.befriend(b, &asker, &format!("ANSW={}", clean_text(answer, 8)));
                }
                if a == Some('B') {
                    self.block(&bp, &group, asker.clone());
                }
                (self.log)(&format!("[online] \"{bp}\" declined \"{asker}\"'s friend request"), Some(&group));
                match self.buddies.online.get(&k).copied() {
                    Some(o) => self.bsend(o, "RNOT", &fields![("USER", bp.as_str()), ("CHNG", "D")], ""),
                    None => Ok(()),
                }
            }
            "PADD" | "PDEL" => {
                // the Messenger's "All" list: players this one searched for or met (in memory only); their presence
                let who = strip_at(f.str("USER")).to_string();
                self.bsend(b, kind, &with_id, "")?;
                if !valid_name(&who) {
                    return Ok(());
                }
                let x = self.buddies.conns.get_mut(&b).expect("held");
                let k = lower(&who);
                if kind == "PDEL" {
                    if x.met.remove(&k)
                        && let Some(s) = self.buddies.met_by.get_mut(&k)
                    {
                        s.remove(&b);
                        if s.is_empty() {
                            self.buddies.met_by.remove(&k);
                        }
                    }
                    return Ok(());
                }
                if x.met.len() < MAX_LIST && x.met.insert(k.clone()) {
                    self.buddies.met_by.entry(k.clone()).or_default().insert(b);
                }
                let p = self.presence_of(&k, &who);
                self.bsend(b, "PGET", &p, "")
            }
            "USCH" => {
                // Search: personas online now or known from their races whose name starts with the text (at most
                // MAXR, 20)
                let now = self.bnow();
                if !self.buddies.conns.get_mut(&b).expect("held").heavy.take(now, 1.0) {
                    return self.bsend(b, "USCH", &wid(fields![("SIZE", 0)]), "");
                }
                let q = jsx::slice16(&lower(strip_at(f.str("USER"))), 15).to_string();
                let max = 20i32.min(1i32.max(jsx::to_int32(f.num("MAXR", 20.0)))) as usize;
                let mut found: IndexMap<String, String> = IndexMap::new();
                if !q.is_empty() {
                    // (by_persona's keys are its sessions' personas in lower case: none made again for each)
                    for (k, s) in &self.lobby.by_persona {
                        if k.starts_with(&q)
                            && let Some(p) = &self.lobby.s(*s).persona
                        {
                            found.insert(k.clone(), p.clone());
                        }
                    }
                    for n in self.stats.starting_with(&q, max + 1) {
                        found.insert(lower(&n), n);
                    }
                }
                found.shift_remove(&me);
                let mut names: Vec<String> = found.into_values().collect();
                names.sort();
                names.truncate(max);
                self.bsend(b, "USCH", &wid(fields![("SIZE", names.len())]), "")?;
                for n in names {
                    self.bsend(b, "USER", &wid(fields![("USER", n)]), "")?;
                }
                Ok(())
            }
            "PSET" => {
                let show = jsx::slice16(f.get("SHOW").unwrap_or("CHAT"), 8).to_string();
                let stat = jsx::slice16(f.str("STAT"), 64).to_string();
                let prod = clean_text(f.str("PROD"), 64);
                self.bsend(b, "PSET", &with_id, "")?;
                let x = self.buddies.conns.get_mut(&b).expect("held");
                if x.told.is_some() && show == x.show && stat == x.stat && prod == x.prod {
                    return Ok(());
                }
                x.show = show;
                x.stat = stat;
                x.prod = prod;
                self.presence_changed(b)
            }
            "SEND" | "GINV" | "GRSP" | "GRVK" => {
                let to = strip_at(f.str("USER")).to_string();
                let k = lower(&to);
                let o = self.buddies.online.get(&k).copied();
                let t = self.bnow();
                let x = self.buddies.conns.get_mut(&b).expect("held");
                // messages and invites: 2 a second, a burst of 5 (over that: "full", the connection stays)
                x.msgs_v = 5f64.min(x.msgs_v + (t - x.msgs_t) / 500.0);
                x.msgs_t = t;
                if x.msgs_v < 1.0 {
                    return self.bsend(b, kind, &with_id, "full");
                }
                x.msgs_v -= 1.0;
                self.bsend(b, kind, &with_id, "")?;
                // away: a message from a friend waits for the persona (an invite is forgotten)
                let Some(o) = o.filter(|o| self.buddies.conns.get(o).is_some_and(|x| x.persona.is_some())) else {
                    if kind == "SEND" && valid_name(&to) {
                        self.keep_mail(&k, &bp, f, t);
                    }
                    return Ok(());
                };
                // this sender is blocked
                if self.buddies.blocks(&k, &me) {
                    return Ok(());
                }
                let bp2 = self.buddies.conns.get(&b).and_then(|x| x.persona.clone());
                if kind == "SEND" {
                    let rec = fields![
                        ("USER", bp2),
                        ("BODY", clean_text(f.str("BODY"), 500)),
                        ("SUBJ", clean_text(f.str("SUBJ"), 64)),
                        ("TYPE", jsx::slice16(f.get("TYPE").unwrap_or("C"), 4)),
                        ("TIME", (t / 1000.0).floor())
                    ];
                    self.bsend(o, "RECV", &rec, "")
                } else {
                    // a game invite and its answers: the game's own fields, the sender named by the server
                    let mut rec = fields![("USER", bp2)];
                    let passed =
                        f.0.iter()
                            .filter_map(|(k, v)| Some((k, v.as_ref()?)))
                            .filter(|(k, _)| !["USER", "ID", "LKEY"].contains(&k.as_str()));
                    rec.extend(passed.take(12).map(|(k, v)| (std::borrow::Cow::from(k.clone()), V::S(clean_text(v, 128)))));
                    self.bsend(o, kind, &rec, "")
                }
            }
            _ => self.bsend(b, &short, &with_id, ""),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // buddies.json read leniently and written again, with room for 3 lists
    #[test]
    fn buddies_json_read_and_written_again() {
        let friends: Vec<String> = (0..60).map(|i| format!("\"Friend{i}\"")).collect();
        let many = format!(r#"{{"personas":[{{"name":"Many","seen":"2026-03","B":[{}]}}]}}"#, friends.join(","));
        let fifty = format!(r#"{{"name":"Many","seen":"2026-03","B":[{}]}}"#, friends[..50].join(","));
        let mail: Vec<String> = (0..25).map(|i| format!(r#"{{"from":"Friend1","t":{i},"body":"m{i}","subj":"","type":"C"}}"#)).collect();
        let lots = format!(r#"{{"v":3,"personas":[{{"name":"Inbox","seen":"2026-10","B":["Friend1"],"M":[{}]}}]}}"#, mail.join(","));
        let twenty = format!(r#"{{"name":"Inbox","seen":"2026-10","B":["Friend1"],"M":[{}]}}"#, mail[5..].join(","));
        let cases = [
            ("{not json", ""),
            ("", ""),
            (
                r#"{"v":2,"note":"NFSU2 online friends lists (persona names only)","personas":[{"name":"Alpha","seen":"2026-10","B":["Bravo","Charlie"]}]}"#,
                r#"{"name":"Alpha","seen":"2026-10","B":["Bravo","Charlie"]}"#,
            ),
            // a friend twice: the later spelling in the earlier one's place; friends that are no name; the blocked
            // list of a file of version 2 (I) not read; an owner twice: the more recently seen list, in the other
            // one's place
            (
                r#"{"personas":[{"name":"Alpha","seen":"2026-10","B":["Bravo","bravo","x y",5,null,"Delta"],"I":["Mallory"]},{"name":"alpha","seen":"2026-09","B":["Echo"]}]}"#,
                r#"{"name":"Alpha","seen":"2026-10","B":["bravo","Delta"]}"#,
            ),
            // owners that are no name not read; of the others the 3 most recently seen (no month: the oldest), of
            // those the lists with anything in them
            (
                r#"{"personas":[{"name":"NoFriends","seen":"2026-10","B":[]},{"name":"Bad Name","B":["Alpha"]},{"name":"Old","B":["Alpha"]},{"name":"Mid","seen":"2026-05","B":["Alpha"]},{"name":"New","seen":"2026-12","B":"Alpha"},{"name":"Newer","seen":"2027-01","B":["Zed"]}]}"#,
                r#"{"name":"Newer","seen":"2027-01","B":["Zed"]}"#,
            ),
            // 50 friends at most
            (many.as_str(), fifty.as_str()),
            // version 3: the blocked, the requests sent (ones that are no name or no time left out), the messages
            // waiting (text cleaned, ones from no name or without a time left out); a list with only some of them
            (
                r#"{"v":3,"personas":[{"name":"Alpha","seen":"2026-10","B":["Bravo"],"I":["Mallory","x y"],"R":[["Charlie",1761200000000],["x y",1],["Delta"],["Echo",-5]],"M":[{"from":"Bravo","t":1761200000123.7,"body":"hi\u0007 there","subj":"","type":"C"},{"from":"x y","t":1,"body":"no"},{"from":"Bravo","body":"no time"}]},{"name":"Lonely","seen":"2026-10","B":[],"R":[["Alpha",5]]}]}"#,
                r#"{"name":"Alpha","seen":"2026-10","B":["Bravo"],"I":["Mallory"],"R":[["Charlie",1761200000000]],"M":[{"from":"Bravo","t":1761200000123,"body":"hi there","subj":"","type":"C"}]},{"name":"Lonely","seen":"2026-10","B":[],"R":[["Alpha",5]]}"#,
            ),
            // the newest 20 messages
            (lots.as_str(), twenty.as_str()),
        ];
        for (text, kept) in cases {
            let dir = tempfile::tempdir().unwrap();
            std::fs::write(dir.path().join("buddies.json"), text).unwrap();
            let b = Buddies::new(Some(dir.path().to_path_buf()), BuddyLimits { lists: 3.0, ..Default::default() });
            let want = format!(
                r#"{{"v":3,"note":"NFSU2 online Messenger lists (persona names; friend requests and messages waiting for their persona)","personas":[{kept}]}}"#
            );
            assert_eq!(b.serialize(), want, "{text}");
        }
    }
}
