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
//! - RADD/RDEL LIST=I USER ID -> ack: blocking, kept with the persona's lists; RADD LIST=B (the game sends none) is a
//!   friend request, as RADM (until the 2026-11-05 security audit it put a friend on this persona's list only, unasked)
//! - PADD/PDEL USER -> ack, and that player's presence (the Messenger's "All" list: players searched for or met; the
//!   game's own, in memory only)
//! - PSET SHOW STAT PROD -> ack; when it changed, PGET USER SHOW STAT PROD TITL EXTR ATTR to every online player who
//!   has this persona as a friend (at most once a second, the latest)
//! - SEND USER BODY SUBJ TYPE -> SEND; RECV USER (the sender) BODY SUBJ TYPE TIME to the persona; to one that is away
//!   and has the sender as a friend it goes to that persona's pages (`PageMailHook`), else waits (`MAIL_DAYS`) for
//!   its next login or page.  One that reads as a slur reaches no one (`msg_slur`: the lobby chat's rule)
//! - GINV/GRSP/GRVK USER ... -> ack; passed on to the persona (USER = the sender)
//! - USCH USER MAXR ID -> USCH ID SIZE=n, then n x USER ID USER (the Search: personas online or known from their
//!   races whose names start with the text)
//! - PING -> PING;  DISC -> closed
//!
//! Field names and the roster's bits from the game's own code (0x74C640 ROST, 0x74E2B0 RNOT, 0x74D420 the adds,
//! 0x74D660 the deletes); how its screens show each was checked only in part.
//!
//! Kept in <state>/online/friends/, one file a persona (`Shelf`), so they hold over a restart: each persona's friends
//! and blocked (names), the friend requests it sent that wait for an answer and the ones that wait for its own, and
//! the messages from friends that wait for it; but only for a claimed name (nfsu2-claims), with the keyphrase that
//! claimed it (`owner`).  A list is read when its persona needs it (logs in to the lobby or the Messenger, is asked,
//! befriended or sent a message, its page asks for its friends) and written when it changed, at most every
//! `save_ms`: the store's size is the players who play, not the players who ever did (one buddies.json, written whole
//! every 10 s from this thread, held 5000 lists at most until 2026-10-31; a file of then is moved into the shelf at
//! the start).  `lists` keeps the lists read lately in memory (`BuddyLimits::lists` of them, the least recently seen
//! going first, never one of a persona in the lobby or the Messenger), and the indexes (`fans`, `blockers`) are of
//! those: every persona online has its list there, so its friends' presences and blocks reach it.  Lists of claims
//! that ended go from the shelf once an hour (`sweep`).  Before names could be claimed a persona
//! was whoever played under its name, so players who kept the same name (NAME, the default, most of all) saw and
//! changed one another's lists (a player's report, 2026-10-28).  Now:
//! - a claimed name's lists are its owner's: made, read and kept for its keyphrase only (the lobby lets nobody else
//!   play under it).  A list of before (no owner) goes to the first keyphrase that claims its name, and one whose
//!   claim ended goes; NAME's and Player's of before went.  An owner that changed its name finds its lists under the
//!   new one, and its friends find it there on theirs (`move_list`).
//! - a name nobody claimed gets lists for the session (`temp`): never written, gone with the persona.
//!
//! Without claims (OnlineOptions::claims None) every list is kept for its name, as before.  The blocked list was the
//! session's own until 2026-10-23 (the Online audit's ONL-6).  Invites are passed on and forgotten (to a persona
//! online now).  Limits: 50 friends, 50 blocked, `MAX_ASKED` requests waiting from one persona, `MAX_MAIL` messages
//! waiting for one (the oldest goes) and `ALL_MAIL` in the lists in memory; new lists `list_rate` an hour per
//! client; messages `msg_rate` a second (burst 3 x), the lists and the search `heavy_rate` a second (burst 4), over
//! that: closed (the Online audit's ONL-1, ONL-3); chat messages 2 a second (burst 5) and 500 characters; a blocked
//! persona's messages, invites and requests never arrive.
//!
//! Free roam's pages befriend through the same lists (`friends`, the page's keyphrase for the persona: its claimed
//! name), so a friend made by touching front bumpers in free roam is a friend in the Messenger too.  Pages message
//! their friends through the same store (`page_send`, `PAGE_MSG_LEN` characters): to the Messenger of a friend logged
//! in there, else to the friend's pages, else it waits as the Messenger's own do, and a page takes the ones that
//! waited with its list (`FriendAsk::ListMail`).  A page may give its persona's Discord username (`FriendAsk::Discord`:
//! Discord's form only, `discord_name`), which its friends both ways see on their lists, nobody else.
//!
//! A buddy connection's state stays readable until the event that closed it is over, so the code still handling that
//! event goes on with it (a handler whose own answer closed the connection goes on with no persona, and fails where
//! it needs one: `null_persona`).

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use indexmap::IndexMap;
use nfsu2_util::json;
use serde_json::Value;
use sha2::{Digest, Sha256};

use crate::aries::{FieldMap, Fields, FrameReader, V, encode_frame};
use crate::core::Core;
use crate::jsx::{self, clean_text, lower, strip_at, valid_name};
use crate::lobby::{LBucket, four_printable};
use crate::omap::OrderedMap;
use crate::timers::{Action, TimerId};
use crate::vhost::{ConnId, Svc};

const MAX_LIST: usize = 50;
/// friend requests one persona has waiting, and the days one waits for its answer
const MAX_ASKED: usize = 20;
const ASK_DAYS: f64 = 30.0;
/// messages waiting for one persona, for all of them together, and the days one waits
const MAX_MAIL: usize = 20;
const ALL_MAIL: usize = 20_000;
const MAIL_DAYS: f64 = 7.0;
/// a page's message to a friend, at most (characters): three lines of free roam's log (web/src/app/messages.ts)
pub const PAGE_MSG_LEN: usize = 120;
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
const TITL: &str = "Need for Speed Underground 2 [PC]";
/// the folder of the lists in the store's folder, and the file of before it (moved in at the start)
const SHELF: &str = "friends";
const OLD_FILE: &str = "buddies.json";
/// how often the shelf is swept of the lists of claims that ended
const SWEEP_MS: f64 = 3_600_000.0;

/// EA Messenger's limits (see the module's text): `lists` friends lists kept in memory (the shelf keeps them all),
/// `list_rate` new ones an hour per client, `msg_rate` messages a second (burst 3 x), `heavy_rate` lists and searches
/// a second (burst 4), `presence_ms` the least time between two presences of one persona
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
        BuddyLimits { lists: 50_000.0, list_rate: 20.0, msg_rate: 10.0, heavy_rate: 1.0, presence_ms: 1000.0 }
    }
}

/// What a page asks of its persona's lists (`OnlineHost::friends`): its keyphrase says who it is, the keyphrase's
/// claimed name is the persona
#[derive(Clone, Debug)]
pub enum FriendAsk {
    /// the lists as they are
    List,
    /// a request to the persona `name` (free roam: the two cars touched front bumpers, and the switch saw the two
    /// players linked); friends at once when `name` asked first
    Ask(String),
    /// the answer to name's request (true: friends; false: declined)
    Answer(String, bool),
    /// friends no more, and its own request to name taken back
    Remove(String),
    /// name blocked: friends no more, its request gone, and the next ones never arrive
    Block(String),
    /// the lists, and the messages that waited for the persona (`FriendReply::mail`: the page's now, gone from here)
    ListMail,
    /// a message to the friend `name` (`page_send`): at most `PAGE_MSG_LEN` characters
    Send(String, String),
    /// a request to the persona `name` from the start page (its name typed: the server's friends.rs gives the claimed
    /// name as its claim spells it); friends at once when `name` asked first
    AskByName(String),
    /// its Discord username for its friends to see (`discord_name`: Discord's form, not one the name filter refuses;
    /// "" takes it away): "ok", "name" (not one), "full" (no list could be made for it)
    Discord(String),
}

/// A Discord username as Discord allows one (2 to 32 of a-z, 0-9, `_` and `.`, never two `.` together), from what
/// a player typed: the spaces around it, a leading `@` and capitals are let go.  None: not one.  (Its only use is
/// for friends to add each other on Discord: a form this strict carries no message.)
pub fn discord_name(typed: &str) -> Option<String> {
    let t = typed.trim();
    let t = t.strip_prefix('@').unwrap_or(t).to_ascii_lowercase();
    let ok = |c: char| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '.';
    ((2..=32).contains(&t.len()) && t.chars().all(ok) && !t.contains("..")).then_some(t)
}

/// a message for a page (`FriendReply::mail`, `PageMailHook`): who sent it, its text, and when (ms since 1970)
#[derive(Clone, Debug, PartialEq)]
pub struct PageMail {
    pub from: String,
    pub body: String,
    pub t: f64,
}

/// Where a friend's message goes when its persona is not in the Messenger (`Core::set_page_mail`): the persona's
/// pages (the server's friends.rs, the open pages that play with its keyphrase).  True: one took it.  Called on the
/// Online thread with the persona's name as given.
pub type PageMailHook = Box<dyn Fn(&str, &PageMail) -> bool + Send>;

/// one friend as the page sees it
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FriendRow {
    pub name: String,
    /// its list names this persona back, under its name's claim: a friendship both ways, as an accepted request makes
    /// it.  Only these are told where they play (the server's friends.rs) and linked first in free roam; a one-way
    /// entry (a list of before the 2026-11-05 security audit, when the Messenger's RADD LIST=B made one unasked, or a
    /// name its friend let go and another keyphrase claimed) is a name on the list, no more.
    pub mutual: bool,
    /// in the Online lobby or the Messenger now (a mutual friend only)
    pub online: bool,
    /// its Discord username (`FriendsList::contact`; a mutual friend only)
    pub contact: Option<String>,
}

/// What the ask did (`done`), and the persona's lists after it
#[derive(Clone, Debug, Default)]
pub struct FriendReply {
    /// the persona (the keyphrase's claimed name); None: it has none, nothing was done
    pub me: Option<String>,
    /// "ok"; "friends" (now, or already); "asked" (the request waits); "none" (no such request or friend); "full" (a
    /// list is full, or the client made its share of new lists); "self"; "name" (no name); "claim" (the caller, or
    /// the one asked, has no claimed name)
    pub done: &'static str,
    pub friends: Vec<FriendRow>,
    /// the requests that wait for its answer, and its own that wait for theirs
    pub asks: Vec<String>,
    pub sent: Vec<String>,
    /// `FriendAsk::ListMail`: the messages that waited for it, oldest first
    pub mail: Vec<PageMail>,
    /// its own Discord username, for its friends (`FriendAsk::Discord`)
    pub contact: Option<String>,
}

/// a message that waits for its persona: who sent it, its text, and when (ms since 1970)
pub struct Mail {
    pub from: String,
    pub body: String,
    pub subj: String,
    pub kind: String,
    pub t: f64,
}

/// a persona's lists: its name, the friends (lower case -> the name as given; `B` in its file), the blocked (`I`),
/// the friend requests it sent that wait for an answer (lower case -> the name as given and when; `R`), the ones sent
/// to it that wait for its answer (`Q`: the same requests, from the asker's list, so a persona that logs in finds them
/// without a look through every list), the messages that wait for it, the oldest first (`M`), and the month it was
/// last seen
pub struct FriendsList {
    pub name: String,
    pub b: IndexMap<String, String>,
    pub i: IndexMap<String, String>,
    pub r: IndexMap<String, (String, f64)>,
    pub q: IndexMap<String, (String, f64)>,
    pub m: Vec<Mail>,
    pub seen: String,
    /// the keyphrase that claimed the name (nfsu2-claims; `owner` in its file); None: a list of before claims
    pub owner: Option<Arc<str>>,
    /// a list of a name nobody claimed: for its session only, never written
    pub temp: bool,
    /// its Discord username, which its friends see beside its name (`C`; `FriendAsk::Discord`, `discord_name`)
    pub contact: Option<String>,
}

impl FriendsList {
    fn new(name: &str, seen: String) -> FriendsList {
        FriendsList {
            name: name.to_string(),
            b: IndexMap::new(),
            i: IndexMap::new(),
            r: IndexMap::new(),
            q: IndexMap::new(),
            m: Vec::new(),
            seen,
            owner: None,
            temp: false,
            contact: None,
        }
    }

    /// nothing in it to keep
    fn is_empty(&self) -> bool {
        self.b.is_empty() && self.i.is_empty() && self.r.is_empty() && self.q.is_empty() && self.m.is_empty() && self.contact.is_none()
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
    /// the claimed names (the module's text); None: every list kept for its name
    claims: Option<Arc<nfsu2_claims::Claims>>,
    /// lower-case persona -> its friends list (the ones in memory), least recently seen first
    pub lists: OrderedMap<String, FriendsList>,
    /// lower-case persona -> buddy connection
    online: OrderedMap<String, u64>,
    /// lower-case persona -> the lists (their owners, lower case) that have it as a friend, and the connections that
    /// met it: who hears of its presence (so a presence goes to the ones that hear, not through every online persona)
    fans: HashMap<String, HashSet<String>>,
    met_by: HashMap<String, HashSet<u64>>,
    /// lower-case persona -> how many lists block it (the lobby's chat looks at each hearer's list only for a line of
    /// a persona some list blocks)
    blockers: HashMap<String, u32>,
    /// the messages waiting, on the lists in memory
    mail: usize,
    /// where a message goes when its persona is not in the Messenger (`PageMailHook`); None: it waits
    page_mail: Option<PageMailHook>,
    /// lower-case persona -> when its messages that read as a slur went (the last `slur_window_ms` of the lobby's
    /// limits), and until when its messages are off (`msg_slur`)
    msg_slurs: HashMap<String, (Vec<f64>, f64)>,
    /// when requests and messages that waited too long were last let go, and the shelf last swept
    pruned: f64,
    swept: f64,
    /// client group -> bucket: new friends lists
    made: HashMap<Arc<str>, LBucket>,
    timer: Option<TimerId>,
    /// the lists (lower case) that changed since they were last written
    dirty: HashSet<String>,
    pub conns: HashMap<u64, Buddy>,
    graveyard: Vec<u64>,
    next: u64,
}

/// The lists on disk: <dir>/friends/<2 hex>/<32 hex>.json, by the SHA-256 of the persona in lower case (a name is no
/// path, and 256 folders keep each one small at any number of players).  A file is one list as `FriendsList`'s text
/// says, written to a temporary file and renamed over the old one; a list with nothing in it has no file.
struct Shelf;

impl Shelf {
    fn file(dir: &Path, k: &str) -> (PathBuf, PathBuf) {
        let h: String = Sha256::digest(k.as_bytes()).iter().take(16).map(|b| format!("{b:02x}")).collect();
        let sub = dir.join(SHELF).join(&h[..2]);
        let file = sub.join(format!("{h}.json"));
        (sub, file)
    }

    fn read(dir: &Path, k: &str) -> Option<serde_json::Map<String, Value>> {
        let (_, file) = Shelf::file(dir, k);
        let bytes = std::fs::read(file).ok()?;
        crate::jsonparse::parse(&String::from_utf8_lossy(&bytes)).ok()?.as_object().cloned()
    }

    fn write(dir: &Path, k: &str, body: &str) -> Result<(), String> {
        let (sub, file) = Shelf::file(dir, k);
        crate::files::write_atomic(&sub, &file, body.as_bytes())
    }

    fn remove(dir: &Path, k: &str) {
        let _ = std::fs::remove_file(Shelf::file(dir, k).1);
    }
}

/// one list of a file (the shelf's, or buddies.json's of before, whatever its version) read leniently: names that are
/// no name and times that are no time left out, at most MAX_LIST friends and blocked, MAX_ASKED requests each way, the
/// newest MAX_MAIL messages; None: no name of a persona
fn list_of_json(o: &serde_json::Map<String, Value>) -> Option<FriendsList> {
    let name = o.get("name")?.as_str()?;
    if !valid_name(name) {
        return None;
    }
    // (a `seen` that is not a string counts as none: see stats.rs `clean`)
    let seen = o.get("seen").and_then(Value::as_str).filter(|s| jsx::is_month(s)).unwrap_or("1970-01");
    let mut l = FriendsList::new(name, seen.to_string());
    l.owner = o
        .get("owner")
        .and_then(Value::as_str)
        .filter(|o| o.len() == 22 && o.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_'))
        .map(Arc::from);
    read_names(o.get("B"), &mut l.b);
    read_names(o.get("I"), &mut l.i);
    for (key, into) in [("R", &mut l.r), ("Q", &mut l.q)] {
        if let Some(Value::Array(a)) = o.get(key) {
            for e in a.iter().take(MAX_ASKED) {
                if let Some([n, t]) = e.as_array().map(Vec::as_slice)
                    && let Some(n) = n.as_str().filter(|n| valid_name(n))
                    && let Some(t) = when(t)
                {
                    into.insert(lower(n), (n.to_string(), t));
                }
            }
        }
    }
    if let Some(Value::Array(a)) = o.get("M") {
        // (the newest MAX_MAIL)
        for e in a.iter().skip(a.len().saturating_sub(MAX_MAIL)) {
            let Some(o) = e.as_object() else { continue };
            let (Some(from), Some(t)) = (o.get("from").and_then(Value::as_str).filter(|n| valid_name(n)), o.get("t").and_then(when)) else {
                continue;
            };
            let text = |k: &str, n: usize| o.get(k).and_then(Value::as_str).map_or(String::new(), |s| clean_text(s, n));
            let kind = o.get("type").and_then(Value::as_str).map_or("C", |s| jsx::slice16(s, 4)).to_string();
            l.m.push(Mail { from: from.to_string(), body: text("body", 500), subj: text("subj", 64), kind, t });
        }
    }
    l.contact = o.get("C").and_then(Value::as_str).and_then(discord_name);
    Some(l)
}

/// one list's text, as the shelf keeps it (`B` always, the others when not empty)
fn list_json(l: &FriendsList) -> String {
    let mut s = String::from("{\"v\":4,\"name\":");
    json::write_str(&mut s, &l.name);
    s.push_str(",\"seen\":");
    json::write_str(&mut s, &l.seen);
    if let Some(o) = &l.owner {
        s.push_str(",\"owner\":");
        json::write_str(&mut s, o);
    }
    write_names(&mut s, "B", l.b.values());
    if !l.i.is_empty() {
        write_names(&mut s, "I", l.i.values());
    }
    for (key, reqs) in [("R", &l.r), ("Q", &l.q)] {
        if reqs.is_empty() {
            continue;
        }
        s.push_str(&format!(",\"{key}\":["));
        for (i, (n, t)) in reqs.values().enumerate() {
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
    if let Some(c) = &l.contact {
        s.push_str(",\"C\":");
        json::write_str(&mut s, c);
    }
    s.push('}');
    s
}

/// The shelf swept of what nobody will read again: the lists whose claim ended (their keyphrase holds the name no
/// more), and lists of before claims of the names nobody claims (NAME, Player).  `skip` says which lists are in
/// memory (left alone: the store decides for those).  Runs on a thread of its own (an hour apart): at hundreds of
/// thousands of lists it reads them all.  The number of files that went.
pub fn sweep(dir: &Path, claims: &nfsu2_claims::Claims, now: i64, skip: &HashSet<String>) -> usize {
    let mut gone = 0;
    let Ok(subs) = std::fs::read_dir(dir.join(SHELF)) else { return 0 };
    for sub in subs.flatten() {
        let Ok(files) = std::fs::read_dir(sub.path()) else { continue };
        for f in files.flatten() {
            let path = f.path();
            if path.extension().is_none_or(|e| e != "json") {
                continue;
            }
            let Ok(bytes) = std::fs::read(&path) else { continue };
            let Ok(o) = crate::jsonparse::parse(&String::from_utf8_lossy(&bytes)) else { continue };
            let Some(name) = o.get("name").and_then(Value::as_str) else { continue };
            if skip.contains(&lower(name)) {
                continue;
            }
            let owner = o.get("owner").and_then(Value::as_str);
            let dead = match owner {
                Some(owner) => claims.holder(name, now).as_deref() != Some(owner),
                None => nfsu2_claims::canonical(name).is_some_and(|n| nfsu2_claims::UNCLAIMABLE.contains(&n.as_str())),
            };
            if dead && std::fs::remove_file(&path).is_ok() {
                gone += 1;
            }
        }
    }
    gone
}

/// every list on the shelf of `dir`, as written, by name (each file where its name says)
#[cfg(test)]
pub(crate) fn shelf_texts(dir: &Path) -> Vec<String> {
    let mut out: Vec<(String, String)> = Vec::new();
    for sub in std::fs::read_dir(dir.join(SHELF)).into_iter().flatten().flatten() {
        for f in std::fs::read_dir(sub.path()).unwrap().flatten() {
            let text = std::fs::read_to_string(f.path()).unwrap();
            let name = crate::jsonparse::parse(&text).unwrap()["name"].as_str().unwrap().to_string();
            assert_eq!(f.path(), Shelf::file(dir, &lower(&name)).1, "{name}'s file is where its name says");
            out.push((name, text));
        }
    }
    out.sort();
    out.into_iter().map(|(_, t)| t).collect()
}

/// the same lists read and written again
#[cfg(test)]
pub(crate) fn shelf_reread(dir: &Path) -> Vec<String> {
    shelf_texts(dir)
        .iter()
        .map(|t| list_json(&list_of_json(crate::jsonparse::parse(t).unwrap().as_object().unwrap()).unwrap()))
        .collect()
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
    #[cfg(test)]
    pub fn new(dir: Option<PathBuf>, l: BuddyLimits) -> Buddies {
        Buddies::with_claims(dir, l, None, 0)
    }

    /// the store of `dir` (none: in memory) as of `now` (ms since 1970: the claims that hold then); a buddies.json of
    /// before in it is moved onto the shelf first
    pub fn with_claims(dir: Option<PathBuf>, l: BuddyLimits, claims: Option<Arc<nfsu2_claims::Claims>>, now: i64) -> Buddies {
        let mut b = Buddies {
            l,
            dir,
            save_ms: 10_000.0,
            claims,
            lists: OrderedMap::new(),
            online: OrderedMap::new(),
            fans: HashMap::new(),
            met_by: HashMap::new(),
            blockers: HashMap::new(),
            mail: 0,
            page_mail: None,
            msg_slurs: HashMap::new(),
            pruned: f64::NEG_INFINITY,
            swept: now as f64,
            made: HashMap::new(),
            timer: None,
            dirty: HashSet::new(),
            conns: HashMap::new(),
            graveyard: Vec::new(),
            next: 0,
        };
        if let Some(dir) = b.dir.clone() {
            b.move_in(&dir, now);
        }
        b
    }

    /// buddies.json (one file of every list, before the shelf) onto the shelf, then renamed to buddies.json.moved: its
    /// lists read as they were then (a name twice: the later list; the lists of claims that ended, and NAME's and
    /// Player's of before claims, left out), each request also on the asked persona's list (`Q`)
    fn move_in(&mut self, dir: &Path, now: i64) {
        let old = dir.join(OLD_FILE);
        let Ok(bytes) = std::fs::read(&old) else { return };
        let mut rows: Vec<FriendsList> = Vec::new();
        if let Ok(j) = crate::jsonparse::parse(&String::from_utf8_lossy(&bytes)) {
            for o in j.get("personas").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_object) {
                let Some(l) = list_of_json(o).filter(|l| !l.is_empty()) else { continue };
                if let Some(c) = &self.claims {
                    let ended = match &l.owner {
                        Some(o) => c.holder(&l.name, now).as_deref() != Some(&**o),
                        None => nfsu2_claims::canonical(&l.name).is_some_and(|n| nfsu2_claims::UNCLAIMABLE.contains(&n.as_str())),
                    };
                    if ended {
                        continue;
                    }
                }
                rows.push(l);
            }
        }
        // (a name twice: the more recently seen list, in the other one's place)
        rows.sort_by(|a, b| a.seen.cmp(&b.seen));
        let mut lists: IndexMap<String, FriendsList> = IndexMap::new();
        for l in rows {
            lists.insert(lower(&l.name), l);
        }
        let asks: Vec<(String, String, String, f64)> =
            lists.values().flat_map(|l| l.r.iter().map(|(k, (n, t))| (k.clone(), n.clone(), l.name.clone(), *t))).collect();
        for (k, name, from, t) in asks {
            let l = lists.entry(k).or_insert_with(|| {
                let mut l = FriendsList::new(&name, jsx::month_of(now as f64));
                l.owner = self.claims.as_ref().and_then(|c| c.holder(&name, now));
                l
            });
            if l.q.len() < MAX_ASKED {
                l.q.insert(lower(&from), (from, t));
            }
        }
        for (k, l) in &lists {
            if Shelf::write(dir, k, &list_json(l)).is_err() {
                return;
            }
        }
        let _ = std::fs::rename(&old, dir.join(format!("{OLD_FILE}.moved")));
    }

    /// the store's indexes learn of the list of `owner` (lower case), which is in `lists`
    fn index(&mut self, owner: &str) {
        let Some(l) = self.lists.get(owner) else { return };
        for k in l.b.keys() {
            self.fans.entry(k.clone()).or_default().insert(owner.to_string());
        }
        for k in l.i.keys() {
            *self.blockers.entry(k.clone()).or_default() += 1;
        }
        self.mail += l.m.len();
    }

    /// the list of `k` (lower case) changed: written at the next save
    fn mark(&mut self, k: &str) {
        if self.dir.is_some() {
            self.dirty.insert(k.to_string());
        }
    }

    /// The list of `k` (lower case) in memory, read from the shelf if it is there (true): a list whose claim ended goes
    /// from it, and one of before claims (no owner) stays there for its name's first keyphrase (`dormant`).  Room is
    /// made for it first: the least recently seen lists go from memory (written first when they changed), but never
    /// one that `pinned` holds or of a persona online in the Messenger.
    fn fetch(&mut self, k: &str, now: f64, pinned: &dyn Fn(&str) -> bool) -> bool {
        if self.lists.contains_key(k) {
            return true;
        }
        let Some(dir) = self.dir.clone() else { return false };
        let Some(l) = Shelf::read(&dir, k).and_then(|o| list_of_json(&o)).filter(|l| lower(&l.name) == k) else { return false };
        if let Some(c) = &self.claims {
            match &l.owner {
                Some(o) if c.holder(&l.name, now as i64).as_deref() != Some(&**o) => {
                    Shelf::remove(&dir, k);
                    return false;
                }
                Some(_) => {}
                None => {
                    if nfsu2_claims::canonical(&l.name).is_some_and(|n| nfsu2_claims::UNCLAIMABLE.contains(&n.as_str())) {
                        Shelf::remove(&dir, k);
                    }
                    return false;
                }
            }
        }
        self.make_room(pinned);
        self.lists.insert(k.to_string(), l);
        self.index(k);
        if self.prune_one(k, now) {
            self.mark(k);
        }
        true
    }

    /// k's list of before claims (no owner) from the shelf, for its name's first keyphrase
    fn dormant(&self, k: &str) -> Option<FriendsList> {
        let dir = self.dir.as_ref()?;
        Shelf::read(dir, k).and_then(|o| list_of_json(&o)).filter(|l| l.owner.is_none() && lower(&l.name) == k)
    }

    /// room for one more list in memory (see `fetch`)
    fn make_room(&mut self, pinned: &dyn Fn(&str) -> bool) {
        while self.lists.len() as f64 >= self.l.lists {
            let Some(old) = self.lists.iter().map(|(k, _)| k).find(|k| !pinned(k) && !self.online.contains_key(*k)).cloned() else {
                return;
            };
            if self.dirty.remove(&old) {
                self.write_one(&old);
            }
            self.take_list(&old);
        }
    }

    /// k's list onto the shelf as it is now (one with nothing in it off the shelf; a session's own never)
    fn write_one(&self, k: &str) -> Option<String> {
        let (dir, l) = (self.dir.as_ref()?, self.lists.get(k)?);
        if l.temp {
            return None;
        }
        if l.is_empty() {
            Shelf::remove(dir, k);
            return None;
        }
        Shelf::write(dir, k, &list_json(l)).err()
    }

    /// the lists that changed, onto the shelf: the first error, if any (the list is written again next time)
    fn write_dirty(&mut self) -> Option<String> {
        let mut err = None;
        for k in std::mem::take(&mut self.dirty) {
            if let Some(e) = self.write_one(&k) {
                self.dirty.insert(k);
                err.get_or_insert(e);
            }
        }
        err
    }

    /// a list into the store, at the most recently seen end
    fn put_list(&mut self, k: String, l: FriendsList) {
        self.take_list(&k);
        self.lists.insert(k.clone(), l);
        self.index(&k);
        self.mark(&k);
    }

    /// the persona `old`'s list (lower case) is `name`'s now: its owner changed its name (nfsu2-claims), and the lists
    /// that had it as a friend, and the requests to and from it, have the new name instead (the ones in memory: the
    /// caller reads its friends' and askers' lists first, and name's own and its friends' and askers').  A list name
    /// has already (a request to the new name, a friend made as it, before the move; the caller made sure it is the
    /// same keyphrase's) takes old's in, within a list's limits: what does not fit (the new name's last friends, the
    /// oldest requests) goes from both sides, so no friendship is left one-sided.
    fn move_list(&mut self, old: &str, name: &str) {
        let Some(mut l) = self.take_list(old) else { return };
        if let Some(dir) = &self.dir {
            Shelf::remove(dir, old);
        }
        self.dirty.remove(old);
        let k = lower(name);
        l.name = name.to_string();
        let (sent, got): (Vec<String>, Vec<String>) = (l.r.keys().cloned().collect(), l.q.keys().cloned().collect());
        if let Some(cur) = self.take_list(&k) {
            l.b.extend(cur.b);
            l.i.extend(cur.i);
            // (a request both had: the later)
            for (m, theirs) in [(&mut l.r, cur.r), (&mut l.q, cur.q)] {
                for (x, (n, t)) in theirs {
                    if m.get(&x).is_none_or(|(_, was)| *was < t) {
                        m.insert(x, (n, t));
                    }
                }
            }
            l.m.extend(cur.m);
            l.contact = cur.contact.or(l.contact);
            l.seen = cur.seen;
        }
        // (the player itself under either name is no friend or request of its own)
        for x in [old, k.as_str()] {
            l.b.shift_remove(x);
            l.i.shift_remove(x);
            l.r.shift_remove(x);
            l.q.shift_remove(x);
        }
        // the limits a list is read back with (list_of_json): old's own first, the newest requests and messages
        let unfriended: Vec<String> = l.b.keys().skip(MAX_LIST).cloned().collect();
        l.b.truncate(MAX_LIST);
        l.i.truncate(MAX_LIST);
        let newest = |m: &mut IndexMap<String, (String, f64)>| -> Vec<String> {
            if m.len() <= MAX_ASKED {
                return Vec::new();
            }
            m.sort_by(|_, (_, a), _, (_, b)| b.total_cmp(a));
            let cut = m.keys().skip(MAX_ASKED).cloned().collect();
            m.truncate(MAX_ASKED);
            cut
        };
        let unasked = [newest(&mut l.r), newest(&mut l.q)];
        if l.m.len() > MAX_MAIL {
            l.m.sort_by(|a, b| a.t.total_cmp(&b.t));
            l.m.drain(..l.m.len() - MAX_MAIL);
        }
        self.put_list(k.clone(), l);
        let rename = |m: &mut IndexMap<String, (String, f64)>| match m.shift_remove(old) {
            Some((_, t)) => {
                m.insert(k.clone(), (name.to_string(), t));
                true
            }
            None => false,
        };
        let mut changed: Vec<String> = Vec::new();
        for (who, theirs_q) in sent.iter().map(|w| (w, true)).chain(got.iter().map(|w| (w, false))) {
            if let Some(wl) = self.lists.get_mut(who)
                && rename(if theirs_q { &mut wl.q } else { &mut wl.r })
            {
                changed.push(who.clone());
            }
        }
        if let Some(fans) = self.fans.remove(old) {
            for f in &fans {
                if let Some(fl) = self.lists.get_mut(f)
                    && fl.b.shift_remove(old).is_some()
                {
                    fl.b.insert(k.clone(), name.to_string());
                    changed.push(f.clone());
                }
            }
            self.fans.entry(k.clone()).or_default().extend(fans);
        }
        // what did not fit, off the other side too (by now every one of them names k)
        for x in &unfriended {
            if self.remove_friend(x, &k) {
                changed.push(x.clone());
            }
        }
        for (cut, theirs_q) in [(&unasked[0], true), (&unasked[1], false)] {
            for x in cut {
                if let Some(wl) = self.lists.get_mut(x)
                    && (if theirs_q { &mut wl.q } else { &mut wl.r }).shift_remove(&k).is_some()
                {
                    changed.push(x.clone());
                }
            }
        }
        for f in changed {
            self.mark(&f);
        }
    }

    /// a list out of memory (and the indexes): not off the shelf
    fn take_list(&mut self, owner: &str) -> Option<FriendsList> {
        let l = self.lists.get(owner)?;
        let (b, i, m): (Vec<String>, Vec<String>, usize) = (l.b.keys().cloned().collect(), l.i.keys().cloned().collect(), l.m.len());
        for k in &b {
            self.unfan(k, owner);
        }
        for k in &i {
            self.blocked_less(k);
        }
        self.mail -= m;
        self.lists.remove(owner)
    }

    /// a friend on owner's list (false: it has none)
    fn add_friend(&mut self, owner: &str, k: String, name: String) -> bool {
        let Some(l) = self.lists.get_mut(owner) else { return false };
        l.b.insert(k.clone(), name);
        self.fans.entry(k).or_default().insert(owner.to_string());
        self.mark(owner);
        true
    }

    /// a friend off owner's list (false: it was not on it)
    fn remove_friend(&mut self, owner: &str, k: &str) -> bool {
        if !self.lists.get_mut(owner).is_some_and(|l| l.b.shift_remove(k).is_some()) {
            return false;
        }
        self.unfan(k, owner);
        self.mark(owner);
        true
    }

    /// k (lower case, `name` as given) on owner's blocked list (false: it has no list, or k was on it)
    fn block_add(&mut self, owner: &str, k: String, name: String) -> bool {
        let Some(l) = self.lists.get_mut(owner) else { return false };
        if l.i.insert(k.clone(), name).is_some() {
            return false;
        }
        self.blocked_more(&k);
        self.mark(owner);
        true
    }

    /// k off owner's blocked list (false: it was not on it)
    fn block_remove(&mut self, owner: &str, k: &str) -> bool {
        if !self.lists.get_mut(owner).is_some_and(|l| l.i.shift_remove(k).is_some()) {
            return false;
        }
        self.blocked_less(k);
        self.mark(owner);
        true
    }

    /// owner (lower case) blocks k
    pub fn blocks(&self, owner: &str, k: &str) -> bool {
        self.lists.get(owner).is_some_and(|l| l.i.contains_key(k))
    }

    /// some list blocks k (lower case)
    pub fn blocked(&self, k: &str) -> bool {
        self.blockers.contains_key(k)
    }

    /// k (lower case) on one more blocked list, or on one less
    fn blocked_more(&mut self, k: &str) {
        *self.blockers.entry(k.to_string()).or_default() += 1;
    }
    fn blocked_less(&mut self, k: &str) {
        if let Some(n) = self.blockers.get_mut(k) {
            *n -= 1;
            if *n == 0 {
                self.blockers.remove(k);
            }
        }
    }

    /// from's friend request to `to` (both lower case) while it waits: the asker's name (from the asked one's list
    /// when it is in memory, else the asker's)
    fn request(&self, from: &str, to: &str, now: f64) -> Option<&str> {
        let (name, t) = match self.lists.get(to) {
            Some(l) => {
                let (n, t) = l.q.get(from)?;
                (n.as_str(), *t)
            }
            None => {
                let l = self.lists.get(from)?;
                (l.name.as_str(), l.r.get(to)?.1)
            }
        };
        (now - t < ASK_DAYS * DAY_MS).then_some(name)
    }

    /// from's request to `to` (lower case `to`, its name as given), on from's list (which is there) and on to's (when
    /// it is in memory: the caller makes it for a claimed name)
    fn ask(&mut self, from: &str, to: String, name: String, t: f64) {
        let Some(l) = self.lists.get_mut(from) else { return };
        l.r.insert(to.clone(), (name, t));
        let from_name = l.name.clone();
        self.mark(from);
        if let Some(tl) = self.lists.get_mut(&to) {
            tl.q.insert(from.to_string(), (from_name, t));
            if tl.q.len() > MAX_ASKED {
                tl.q.shift_remove_index(0);
            }
            self.mark(&to);
        }
    }

    /// from's request to `to` goes, from both lists (false: there was none)
    fn unask(&mut self, from: &str, to: &str) -> bool {
        let sent = self.lists.get_mut(from).is_some_and(|l| l.r.shift_remove(to).is_some());
        let got = self.lists.get_mut(to).is_some_and(|l| l.q.shift_remove(from).is_some());
        if sent {
            self.mark(from);
        }
        if got {
            self.mark(to);
        }
        sent || got
    }

    /// a list goes, from memory and the shelf (its claim ended)
    fn drop_list(&mut self, owner: &str) {
        self.take_list(owner);
        self.dirty.remove(owner);
        if let Some(dir) = &self.dir {
            Shelf::remove(dir, owner);
        }
    }

    /// the lists of names nobody claimed whose persona is not online go (they are for its session only): true when
    /// any did
    fn drop_temp(&mut self) -> bool {
        let gone: Vec<String> =
            self.lists.iter().filter(|(k, l)| l.temp && !self.online.contains_key(*k)).map(|(k, _)| k.clone()).collect();
        for k in &gone {
            // (never on the shelf: a file under its name is a list of before claims, left for its first keyphrase)
            self.take_list(k);
            self.dirty.remove(k);
        }
        !gone.is_empty()
    }

    fn unfan(&mut self, k: &str, owner: &str) {
        if let Some(s) = self.fans.get_mut(k) {
            s.remove(owner);
            if s.is_empty() {
                self.fans.remove(k);
            }
        }
    }

    /// the requests and messages that waited too long go, from the lists in memory (false: none did)
    fn prune(&mut self, now: f64) -> bool {
        let keys: Vec<String> = self.lists.iter().map(|(k, _)| k.clone()).collect();
        let mut any = false;
        for k in keys {
            if self.prune_one(&k, now) {
                self.mark(&k);
                any = true;
            }
        }
        any
    }

    /// the requests (both ways) and messages of k's list that waited too long go (false: none did)
    fn prune_one(&mut self, k: &str, now: f64) -> bool {
        let Some(l) = self.lists.get_mut(k) else { return false };
        let (r, q, m) = (l.r.len(), l.q.len(), l.m.len());
        l.r.retain(|_, (_, t)| now - *t < ASK_DAYS * DAY_MS);
        l.q.retain(|_, (_, t)| now - *t < ASK_DAYS * DAY_MS);
        l.m.retain(|m| now - m.t < MAIL_DAYS * DAY_MS);
        let mail = m - l.m.len();
        self.mail -= mail;
        r != l.r.len() || q != l.q.len() || mail > 0
    }

    /// k's list (lower case, in memory) when it names `me` (lower case) back and is its name's claimer's: with me's
    /// list naming k, a friendship both ways (`FriendRow::mutual`)
    fn names_back(&self, k: &str, me: &str, now: f64) -> Option<&FriendsList> {
        let f = self.lists.get(k).filter(|f| f.b.contains_key(me) && !f.temp)?;
        match &self.claims {
            // (a claim that changed hands leaves its old list behind for a while)
            Some(c) => (f.owner.is_some() && f.owner == c.holder(&f.name, now as i64)).then_some(f),
            None => Some(f),
        }
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

/// a persona's presence as its friends see it
fn presence(b: &Buddy) -> Fields {
    fields![
        ("USER", &b.persona),
        ("SHOW", &b.show),
        ("STAT", &b.stat),
        ("PROD", &b.prod),
        ("TITL", TITL),
        ("EXTR", "NFS-CONSOLE-2005"),
        ("ATTR", "D")
    ]
}

/// the presence of a persona that is not online
fn offline(name: &str) -> Fields {
    fields![("USER", name), ("SHOW", "DISC"), ("STAT", ""), ("PROD", ""), ("TITL", TITL), ("EXTR", "NFS-CONSOLE-2005"), ("ATTR", "D")]
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
            Some(o) => presence(o),
            None => offline(name),
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
        // (a name nobody claimed: its session's lists go with it)
        self.buddies.drop_temp();
    }

    fn touch_lists(&mut self) {
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

    /// the lists that changed onto the shelf (what waited too long not written: the lists in memory are pruned at
    /// most once an hour, and each when it is read)
    pub fn buddy_save(&mut self) {
        if self.buddies.dir.is_none() {
            return;
        }
        let now = self.bnow();
        if now - self.buddies.pruned >= 3_600_000.0 {
            self.buddies.pruned = now;
            self.buddies.prune(now);
        }
        self.buddies.drop_temp();
        if let Some(e) = self.buddies.write_dirty() {
            (self.log)(&format!("[online] the friends lists could not be written: {e}"), None);
            self.touch_lists();
        }
    }

    pub fn buddy_stop(&mut self) {
        if let Some(t) = self.buddies.timer.take() {
            self.timers.clear(t);
        }
        self.buddy_save();
    }

    /// once an hour (the lobby's tick): the shelf swept of the lists of claims that ended, on a thread of its own
    pub fn buddy_tick(&mut self) {
        let now = self.bnow();
        let (Some(dir), Some(claims)) = (self.buddies.dir.clone(), self.buddies.claims.clone()) else { return };
        if now - self.buddies.swept < SWEEP_MS {
            return;
        }
        self.buddies.swept = now;
        let skip: HashSet<String> = self.buddies.lists.iter().map(|(k, _)| k.clone()).collect();
        std::thread::spawn(move || {
            let gone = sweep(&dir, &claims, now as i64, &skip);
            if gone > 0 {
                nfsu2_util::log::line(&format!("[online] {gone} friends list(s) of claims that ended went from the shelf"));
            }
        });
    }

    /// k's list (lower case) in memory when the shelf has it (`Buddies::fetch`): room made without pushing out the
    /// list of a persona in the lobby
    pub(crate) fn buddy_fetch(&mut self, k: &str) -> bool {
        let now = self.bnow();
        let lobby = &self.lobby.by_persona;
        self.buddies.fetch(k, now, &|x| lobby.contains_key(x))
    }

    /// k's list (lower case) in memory as `buddy_fetch` has it, and whether it names persona `me` (lower case) back
    /// (`Buddies::names_back`); me's own list kept in memory meanwhile
    fn fetch_back(&mut self, me: &str, k: &str) -> bool {
        let now = self.bnow();
        let lobby = &self.lobby.by_persona;
        self.buddies.fetch(k, now, &|x| x == me || lobby.contains_key(x));
        self.buddies.names_back(k, me, now).is_some()
    }

    /// a persona's friends list, made if needed (on behalf of client `group`): false when that client made its share
    /// of new lists this hour; the least recently seen lists go from memory when there are too many
    fn make_list(&mut self, name: &str, group: &Arc<str>) -> bool {
        let k = lower(name);
        if self.buddy_fetch(&k) {
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
        self.new_list(name, now);
        true
    }

    /// a new, empty list for `name` (room made for it)
    fn new_list(&mut self, name: &str, now: f64) {
        let lobby = &self.lobby.by_persona;
        self.buddies.make_room(&|x| lobby.contains_key(x));
        let mut list = FriendsList::new(name, jsx::month_of(now));
        // a claimed name's list is its owner's; one nobody claimed is for its session
        if let Some(c) = &self.buddies.claims {
            list.owner = c.holder(name, now as i64);
            list.temp = list.owner.is_none();
        }
        self.buddies.lists.insert(lower(name), list);
    }

    /// the list a request to `name` waits on (`Q`), made for a claimed name (and any without claims): no list rate,
    /// since an asker has `MAX_ASKED` requests at most (false: none, a name nobody claimed has the request on the
    /// asker's list only)
    fn asked_list(&mut self, name: &str) -> bool {
        let k = lower(name);
        if self.buddy_fetch(&k) {
            return true;
        }
        let now = self.bnow();
        if self.buddies.claims.as_ref().is_some_and(|c| c.holder(name, now as i64).is_none()) {
            return false;
        }
        self.new_list(name, now);
        true
    }

    /// The lists of the persona b logs in as, by the claims: a claimed name's are its owner's (b's lobby session's
    /// keyphrase: the lobby gave the name to nobody else), a list of before claims becomes theirs, one of a claim that
    /// ended goes, and an owner that changed its name finds its lists under the new one; a name nobody claimed has
    /// none but its session's own.
    fn lists_at_login(&mut self, persona: &str, owner: Option<Arc<str>>) {
        let k = lower(persona);
        self.buddy_fetch(&k);
        let Some(c) = self.buddies.claims.clone() else { return };
        let now = self.bnow() as i64;
        let holder = c.holder(persona, now);
        let mine = holder.is_some() && holder == owner;
        let (temp, of) = match self.buddies.lists.get(&k) {
            Some(l) => (l.temp, l.owner.clone()),
            None => (false, None),
        };
        if self.buddies.lists.contains_key(&k) {
            if mine && (temp || of.is_none()) {
                // claimed during its session (or by its first keyphrase): kept from now on
                let l = self.buddies.lists.get_mut(&k).expect("there");
                l.temp = false;
                l.owner = owner.clone();
                self.buddies.mark(&k);
                self.touch_lists();
            } else if (mine && of != owner) || (!mine && !temp) {
                // a list of a claim that ended
                self.buddies.drop_list(&k);
                self.touch_lists();
            }
        }
        let Some(o) = owner.filter(|_| mine) else { return };
        if !self.buddies.lists.contains_key(&k)
            && let Some(mut l) = self.buddies.dormant(&k)
        {
            l.owner = Some(o.clone());
            l.name = persona.to_string();
            let lobby = &self.lobby.by_persona;
            self.buddies.make_room(&|x| lobby.contains_key(x));
            self.buddies.put_list(k, l);
            self.touch_lists();
        }
        self.lists_from_before(persona, &o);
    }

    /// The lists of the name keyphrase `owner` had before its last change (nfsu2-claims `previous`, while that name is
    /// still its own) move to its current name `persona`, into any list it has there already: at its EA Messenger
    /// login, and whenever its page asks for its lists (`friends`), whichever comes first.  (Until 2026-11-05 only the
    /// Messenger's login moved them, and only to a name without a list yet: a player who changed its name on the start
    /// page and never opened the Online menu left its friends under the old name, gone with its claim.)
    fn lists_from_before(&mut self, persona: &str, owner: &str) {
        let Some(c) = self.buddies.claims.clone() else { return };
        let k = lower(persona);
        let Some(old) = c.previous(owner, self.bnow() as i64).filter(|o| *o != k) else { return };
        // both lists the keyphrase's (a session's own under the new name: the move waits for the session's end)
        if !self.own_list(&old, owner) || !self.buddies.lists.contains_key(&old) || !self.own_list(&k, owner) {
            return;
        }
        // (its friends' lists and the ones of its requests each way, under either name, in memory to be renamed)
        let others: Vec<String> = [&old, &k]
            .into_iter()
            .filter_map(|n| self.buddies.lists.get(n))
            .flat_map(|l| l.b.keys().chain(l.r.keys()).chain(l.q.keys()).cloned())
            .collect();
        for o in &others {
            self.buddy_fetch(o);
        }
        self.buddies.move_list(&old, persona);
        (self.log)(&format!("[online] \"{persona}\": EA Messenger lists moved from \"{old}\" (a name change)"), None);
        // (written now: the old name's file is gone already)
        if let Some(e) = self.buddies.write_dirty() {
            (self.log)(&format!("[online] the friends lists could not be written: {e}"), None);
        }
        self.touch_lists();
    }

    /// k's list (lower case), if any, is keyphrase `owner`'s, the claimer of its name, from now on (in memory then):
    /// one of before claims (on the shelf, or in memory without a keyphrase) becomes its, as at a Messenger login; one
    /// of another keyphrase (a claim that ended while the list stayed in memory) goes.  False: a session's own list (a
    /// name nobody claimed when its Messenger logged in), left alone.
    fn own_list(&mut self, k: &str, owner: &str) -> bool {
        if !self.buddy_fetch(k)
            && let Some(mut l) = self.buddies.dormant(k)
        {
            l.owner = Some(Arc::from(owner));
            let lobby = &self.lobby.by_persona;
            self.buddies.make_room(&|x| lobby.contains_key(x));
            self.buddies.put_list(k.to_string(), l);
            self.touch_lists();
            return true;
        }
        let Some(l) = self.buddies.lists.get_mut(k) else { return true };
        match l.owner.as_deref() {
            _ if l.temp => false,
            Some(o) if o == owner => true,
            Some(_) => {
                self.buddies.drop_list(k);
                self.touch_lists();
                true
            }
            None => {
                l.owner = Some(Arc::from(owner));
                self.buddies.mark(k);
                self.touch_lists();
                true
            }
        }
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
            self.buddies.mark(&k);
            self.touch_lists();
        }
    }

    /// every online persona that has b as a friend gets its presence now
    fn buddy_announce(&mut self, b: u64, gone: bool) -> Result<(), String> {
        let x = &self.buddies.conns[&b];
        let Some(p) = x.persona.clone() else { return Err(null_persona()) };
        let me = lower(&p);
        let what = if gone { offline(&p) } else { presence(x) };
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
        self.befriend_as(&bp, &group, who, how).map(|_| ())
    }

    /// persona bp (on behalf of client `group`) and `who` become friends, each one's Messenger told when it is online
    /// (false: one of the lists is full, or the client made its share of new lists)
    fn befriend_as(&mut self, bp: &str, group: &Arc<str>, who: &str, how: &str) -> Result<bool, String> {
        let (k, me) = (lower(who), lower(bp));
        self.buddy_fetch(&k);
        if self.buddies.unask(&k, &me) | self.buddies.unask(&me, &k) {
            self.touch_lists();
        }
        let full_without = |l: Option<&FriendsList>, n: &str| l.is_some_and(|l| l.b.len() >= MAX_LIST && !l.b.contains_key(n));
        if full_without(self.buddies.list(bp), &k) || full_without(self.buddies.list(who), &me) {
            return Ok(false);
        }
        let mine = self.make_list(bp, group);
        let them = mine && self.make_list(who, group);
        if !mine || !them {
            return Ok(false);
        }
        // (a list the second one pushed out of memory takes its friend nowhere: `add_friend` finds no list)
        for (l, n) in [(&me, who), (&k, bp)] {
            self.buddies.add_friend(l, lower(n), n.to_string());
        }
        self.touch_lists();
        (self.log)(&format!("[online] \"{bp}\" and \"{who}\" are friends ({how})"), Some(group));
        let (b, o) = (self.buddies.online.get(&me).copied(), self.buddies.online.get(&k).copied());
        if let Some(b) = b {
            self.bsend(b, "RNOT", &fields![("USER", who), ("CHNG", "A"), ("ATTR", 0)], "")?;
            let p = self.presence_of(&k, who);
            self.bsend(b, "PGET", &p, "")?;
        }
        if let Some(o) = o {
            self.bsend(o, "RNOT", &fields![("USER", bp), ("CHNG", "A"), ("ATTR", 0)], "")?;
            let p = self.presence_of(&me, bp);
            self.bsend(o, "PGET", &p, "")?;
        }
        Ok(true)
    }

    /// What a page asks of its persona's lists (see `FriendAsk`): `owner` is its keyphrase, whose claimed name is the
    /// persona; `group` its client (for the list rate and the log)
    pub fn friends(&mut self, owner: &str, group: &Arc<str>, ask: FriendAsk) -> FriendReply {
        let now = self.bnow();
        let Some(me) = self.buddies.claims.as_ref().and_then(|c| c.mine(owner, now as i64)).map(|m| m.name) else {
            return FriendReply { done: "claim", ..Default::default() };
        };
        let mk = lower(&me);
        self.own_list(&mk, owner);
        self.lists_from_before(&me, owner);
        let mut mail = Vec::new();
        let done = match ask {
            FriendAsk::List => "ok",
            FriendAsk::ListMail => {
                mail = self.take_page_mail(&mk);
                "ok"
            }
            FriendAsk::Send(name, body) => self.page_send(&me, group, &name, &body),
            FriendAsk::Ask(name) => self.page_ask(&me, group, &name, "free roam"),
            FriendAsk::AskByName(name) => self.page_ask(&me, group, &name, "the start page"),
            FriendAsk::Answer(name, yes) => self.page_answer(&me, group, &name, yes),
            FriendAsk::Remove(name) => {
                let k = lower(&name);
                self.buddy_fetch(&k);
                let was = self.buddies.list(&me).is_some_and(|l| l.b.contains_key(&k) || l.r.contains_key(&k));
                let _ = self.unfriend(&me, &name);
                if was { "ok" } else { "none" }
            }
            FriendAsk::Block(name) if valid_name(&name) && lower(&name) != mk => {
                let _ = self.unfriend(&me, &name);
                if self.block(&me, group, name) { "ok" } else { "full" }
            }
            FriendAsk::Block(_) => "name",
            FriendAsk::Discord(typed) => self.set_discord(&me, group, &typed),
        };
        // (each friend's list in memory, for whether it names this persona back)
        let keys: Vec<String> = self.buddies.lists.get(&mk).map(|l| l.b.keys().cloned().collect()).unwrap_or_default();
        for k in &keys {
            self.fetch_back(&mk, k);
        }
        let bs = &self.buddies;
        let l = bs.lists.get(&mk);
        let waits = |t: &f64| now - t < ASK_DAYS * DAY_MS;
        let friends = l
            .into_iter()
            .flat_map(|l| l.b.iter())
            .map(|(k, n)| {
                let back = bs.names_back(k, &mk, now);
                let mutual = back.is_some();
                FriendRow {
                    name: n.clone(),
                    mutual,
                    online: mutual && (bs.online.contains_key(k) || self.lobby.by_persona.contains_key(k)),
                    contact: back.and_then(|f| f.contact.clone()),
                }
            })
            .collect();
        let not_friend = |k: &str| !l.is_some_and(|l| l.b.contains_key(k));
        let asks = l
            .into_iter()
            .flat_map(|l| l.q.iter())
            .filter(|(k, (_, t))| waits(t) && not_friend(k) && !bs.blocks(&mk, k))
            .map(|(_, (n, _))| n.clone())
            .collect();
        let sent = l.into_iter().flat_map(|l| l.r.iter()).filter(|(k, (_, t))| waits(t) && not_friend(k)).map(|(_, (n, _))| n.clone()).collect();
        let contact = l.and_then(|l| l.contact.clone());
        FriendReply { me: Some(me), done, friends, asks, sent, mail, contact }
    }

    /// persona `me`'s Discord username from now on (`FriendAsk::Discord`): what it typed, "" none.  The log says that
    /// it changed, never what it is.
    fn set_discord(&mut self, me: &str, group: &Arc<str>, typed: &str) -> &'static str {
        let to = if typed.trim().is_empty() {
            None
        } else {
            match discord_name(typed) {
                Some(d) if !nfsu2_names::blocked_name(&d) => Some(d),
                _ => return "name",
            }
        };
        let k = lower(me);
        let had = self.buddies.list(me).and_then(|l| l.contact.clone());
        if had == to {
            return "ok";
        }
        if !self.make_list(me, group) {
            return "full";
        }
        let Some(l) = self.buddies.lists.get_mut(&k) else { return "full" };
        l.contact = to.clone();
        self.buddies.mark(&k);
        self.touch_lists();
        let what = if to.is_some() { "set a Discord name" } else { "took its Discord name away" };
        (self.log)(&format!("[online] \"{me}\" {what}"), Some(group));
        "ok"
    }

    /// the page of persona `me` asks `name` to be friends (`FriendAsk::Ask` in free roam, `AskByName` on the start
    /// page: `via` says which, in the log)
    fn page_ask(&mut self, me: &str, group: &Arc<str>, name: &str, via: &str) -> &'static str {
        let (mk, k) = (lower(me), lower(name));
        if !valid_name(name) {
            return "name";
        }
        if k == mk {
            return "self";
        }
        let now = self.bnow();
        if self.buddies.claims.as_ref().is_some_and(|c| c.holder(name, now as i64).is_none()) {
            return "claim";
        }
        // (friends both ways already; a name on this one's list alone is asked as any other)
        let back = self.fetch_back(&mk, &k);
        let (friends, full, asked_full) = match self.buddies.list(me) {
            Some(l) => (
                l.b.contains_key(&k) && back,
                l.b.len() >= MAX_LIST && !l.b.contains_key(&k),
                l.r.len() >= MAX_ASKED && !l.r.contains_key(&k),
            ),
            None => (false, false, false),
        };
        if friends {
            return "friends";
        }
        // two requests that cross make friends
        if let Some(asker) = self.buddies.request(&k, &mk, now).map(str::to_string) {
            let how = if via == "free roam" { "asked back in free roam" } else { "asked back on the start page" };
            return match self.befriend_as(me, group, &asker, how) {
                Ok(true) => "friends",
                _ => "full",
            };
        }
        if full || asked_full || !self.make_list(me, group) {
            return "full";
        }
        // asking one it blocked unblocks it; one that blocks the asker never hears of it
        if self.buddies.block_remove(&mk, &k) {
            self.touch_lists();
        }
        if self.buddies.blocks(&k, &mk) {
            return "asked";
        }
        self.asked_list(name);
        self.buddies.ask(&mk, k.clone(), name.to_string(), now.floor());
        self.touch_lists();
        (self.log)(&format!("[online] \"{me}\" asked \"{name}\" to be friends ({via})"), Some(group));
        if let Some(o) = self.buddies.online.get(&k).copied() {
            let _ = self.bsend(o, "RNOT", &fields![("USER", me), ("CHNG", "A"), ("ATTR", ASKED_ME)], "");
            let p = self.presence_of(&mk, me);
            let _ = self.bsend(o, "PGET", &p, "");
        }
        "asked"
    }

    /// the page of persona `me` answers name's request (`FriendAsk::Answer`)
    fn page_answer(&mut self, me: &str, group: &Arc<str>, name: &str, yes: bool) -> &'static str {
        let (mk, k) = (lower(me), lower(name));
        let now = self.bnow();
        self.buddy_fetch(&k);
        let Some(asker) = self.buddies.request(&k, &mk, now).map(str::to_string) else { return "none" };
        if self.buddies.unask(&k, &mk) {
            self.touch_lists();
        }
        if yes {
            return match self.befriend_as(me, group, &asker, "accepted on the page") {
                Ok(true) => "friends",
                _ => "full",
            };
        }
        (self.log)(&format!("[online] \"{me}\" declined \"{asker}\"'s friend request"), Some(group));
        if let Some(o) = self.buddies.online.get(&k).copied() {
            let _ = self.bsend(o, "RNOT", &fields![("USER", me), ("CHNG", "D")], "");
        }
        "ok"
    }

    /// persona `me_name` and `who` are friends no more (on neither list), and me's request to who is taken back: who
    /// told (RNOT CHNG=D) when that changed anything
    fn unfriend(&mut self, me_name: &str, who: &str) -> Result<(), String> {
        let (me, k) = (lower(me_name), lower(who));
        self.buddy_fetch(&k);
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
        self.buddy_fetch(&me);
        if self.buddies.list(me_name).is_some_and(|l| l.i.len() >= MAX_LIST && !l.i.contains_key(&k)) || !self.make_list(me_name, group) {
            return false;
        }
        self.buddies.block_add(&me, k.clone(), who);
        self.buddy_fetch(&k);
        self.buddies.unask(&k, &me);
        self.touch_lists();
        true
    }

    /// a message to persona k (lower case) that is away waits for it when k has the sender as a friend and does not
    /// block it, while there is room (the oldest of k's goes for it)
    fn keep_mail(&mut self, k: &str, from: &str, f: &FieldMap, t: f64) {
        let body = clean_text(f.str("BODY"), 500);
        // a friend's pages take it first (`PageMailHook`)
        if self.page_mail_to(k, &PageMail { from: from.to_string(), body: body.clone(), t: t.floor() }) {
            return;
        }
        let subj = clean_text(f.str("SUBJ"), 64);
        let kind = jsx::slice16(f.get("TYPE").unwrap_or("C"), 4).to_string();
        self.keep(k, Mail { from: from.to_string(), body, subj, kind, t: t.floor() });
    }

    /// `keep_mail`'s store: false when it is not kept (no room, k blocks the sender or has it on no list)
    fn keep(&mut self, k: &str, m: Mail) -> bool {
        let me = lower(&m.from);
        self.buddy_fetch(k);
        if self.buddies.mail >= ALL_MAIL || self.buddies.blocks(k, &me) {
            return false;
        }
        self.buddies.mark(k);
        let Some(l) = self.buddies.lists.get_mut(k).filter(|l| l.b.contains_key(&me)) else { return false };
        l.m.push(m);
        self.buddies.mail += 1;
        if l.m.len() > MAX_MAIL {
            l.m.remove(0);
            self.buddies.mail -= 1;
        }
        self.touch_lists();
        true
    }

    /// where a friend's message goes when its persona k (lower case) is not in the Messenger: its pages
    /// (`PageMailHook`), when k has the sender as a friend and does not block it.  True: a page took it
    fn page_mail_to(&mut self, k: &str, m: &PageMail) -> bool {
        let me = lower(&m.from);
        self.buddy_fetch(k);
        let Some(name) = self.buddies.lists.get(k).filter(|l| l.b.contains_key(&me)).map(|l| l.name.clone()) else { return false };
        if self.buddies.blocks(k, &me) {
            return false;
        }
        self.buddies.page_mail.as_ref().is_some_and(|f| f(&name, m))
    }

    /// Where a friend's messages go when its persona is not in the Messenger (the server's friends.rs: its pages)
    pub fn set_page_mail(&mut self, f: PageMailHook) {
        self.buddies.page_mail = Some(f);
    }

    /// the messages that waited for persona `me` (lower case), for its page, and gone from the store (a sender blocked
    /// since is left out)
    fn take_page_mail(&mut self, me: &str) -> Vec<PageMail> {
        let Some(l) = self.buddies.lists.get_mut(me).filter(|l| !l.m.is_empty()) else { return Vec::new() };
        let mail = std::mem::take(&mut l.m);
        self.buddies.mail -= mail.len();
        self.buddies.mark(me);
        self.touch_lists();
        let now = self.bnow();
        mail.into_iter()
            .filter(|m| now - m.t < MAIL_DAYS * DAY_MS && !self.buddies.blocks(me, &lower(&m.from)))
            .map(|m| PageMail { from: m.from, body: m.body, t: m.t })
            .collect()
    }

    /// A friend's message of persona `mk` (lower case) that reads as a slur (nfsu2-names `chat_slur`, as the lobby's
    /// chat): it reaches no one (its sender is told it went), and the lobby's `slur_lines` of them in its
    /// `slur_window_ms` turn the persona's messages off for its `slur_mute_ms`, the log says so.  True: it reads as one
    fn msg_slur(&mut self, mk: &str, who: &str, body: &str, group: Option<&Arc<str>>) -> bool {
        let Some(word) = nfsu2_names::chat_slur(body).map(str::to_string) else { return false };
        let (now, l) = (self.bnow(), self.lobby.l);
        let x = self.buddies.msg_slurs.entry(mk.to_string()).or_default();
        x.0.retain(|t| now - t < l.slur_window_ms);
        x.0.push(now);
        let muted = x.0.len() as f64 >= l.slur_lines;
        if muted {
            x.1 = now + l.slur_mute_ms;
            x.0.clear();
        }
        let off = nfsu2_util::js::number_to_string(l.slur_mute_ms / 1000.0);
        let then = if muted { format!("; messages off for {off} s") } else { String::new() };
        let word = serde_json::to_string(&word).expect("a string");
        (self.log)(&format!("[online] \"{who}\": a message with {word} in it, sent to no one{then}"), group.map(|g| &**g));
        true
    }

    /// persona `mk`'s (lower case) messages are off now (`msg_slur`)
    fn msg_muted(&mut self, mk: &str) -> bool {
        let now = self.bnow();
        let Some(x) = self.buddies.msg_slurs.get(mk) else { return false };
        if x.1 > now {
            return true;
        }
        if x.0.iter().all(|t| now - t >= self.lobby.l.slur_window_ms) {
            self.buddies.msg_slurs.remove(mk);
        }
        false
    }

    /// The page of persona `me` sends its friend `name` a message (`FriendAsk::Send`): to the friend's EA Messenger
    /// when it is logged in there, else to its pages (`PageMailHook`), else it waits (`MAIL_DAYS`).  "sent", "kept",
    /// "none" (not friends), "empty", "off" (its messages are off: `msg_slur`), "full" (no room for it to wait)
    fn page_send(&mut self, me: &str, group: &Arc<str>, name: &str, body: &str) -> &'static str {
        let (mk, k) = (lower(me), lower(name));
        let body = clean_text(body, PAGE_MSG_LEN);
        if body.is_empty() {
            return "empty";
        }
        if self.msg_muted(&mk) {
            return "off";
        }
        self.buddy_fetch(&k);
        let Some(to) = self.buddies.list(me).and_then(|l| l.b.get(&k).cloned()) else { return "none" };
        // one that reads as a slur, or to a friend that blocks the sender: its sender sees it go, nobody gets it
        if self.msg_slur(&mk, me, &body, Some(group)) || self.buddies.blocks(&k, &mk) {
            return "sent";
        }
        let now = self.bnow();
        let o = self.buddies.online.get(&k).copied().filter(|o| self.buddies.conns.get(o).is_some_and(|x| x.persona.is_some()));
        let done = if let Some(o) = o {
            let rec = fields![("USER", me), ("BODY", body.as_str()), ("SUBJ", ""), ("TYPE", "C"), ("TIME", (now / 1000.0).floor())];
            if self.bsend(o, "RECV", &rec, "").is_ok() { "sent" } else { "full" }
        } else if self.page_mail_to(&k, &PageMail { from: me.to_string(), body: body.clone(), t: now.floor() }) {
            "sent"
        } else if self.keep(&k, Mail { from: me.to_string(), body, subj: String::new(), kind: "C".into(), t: now.floor() }) {
            "kept"
        } else {
            "full"
        };
        let how = match done {
            "sent" => "got it",
            "kept" => "away, it waits",
            _ => "no room for it",
        };
        (self.log)(&format!("[online] \"{me}\" sent \"{to}\" a message ({how})"), Some(group));
        done
    }

    /// the messages that waited for persona `me` (lower case), to connection b, and gone from the store (a sender
    /// blocked since is left out)
    fn deliver_mail(&mut self, b: u64, me: &str) -> Result<(), String> {
        let Some(l) = self.buddies.lists.get_mut(me).filter(|l| !l.m.is_empty()) else { return Ok(()) };
        let mail = std::mem::take(&mut l.m);
        self.buddies.mail -= mail.len();
        self.buddies.mark(me);
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
            let owner = self.lobby.s(s).owner.clone();
            self.lists_at_login(&persona, owner);
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
                        let mut askers: Vec<&String> = l.into_iter().flat_map(|l| l.q.keys()).collect();
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
            // (RADD LIST=B: a friend request, as RADM below)
            "RADD" | "RDEL" | "RDEM" if kind != "RADD" || list_field(f) == "I" => {
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
                    } else if self.buddies.block_remove(&me, &k) {
                        self.touch_lists();
                    }
                    return self.bsend(b, kind, &with_id, "");
                }
                // a friend taken off, or the request taken back: on neither side any more
                self.bsend(b, kind, &with_id, "")?;
                self.unfriend(&bp, &who)
            }
            "RADM" | "RADD" => {
                // a friend request (Send Friend Request): it waits for the persona's answer, RRSP.  RADD LIST=B too,
                // which the game never sends: until the 2026-11-05 security audit it put the persona on this one's
                // list at once, unasked, and the free-roam hub then told this one where the persona played
                let who = strip_at(f.str("USER")).to_string();
                let k = lower(&who);
                if !valid_name(&who) || k == me {
                    return self.bsend(b, kind, &with_id, "user");
                }
                let now = self.bnow();
                // (friends both ways already; a name on this one's list alone is asked as any other)
                let back = self.fetch_back(&me, &k);
                let (friends, full, asked_full) = match self.buddies.list(&bp) {
                    Some(l) => (
                        l.b.contains_key(&k) && back,
                        l.b.len() >= MAX_LIST && !l.b.contains_key(&k),
                        l.r.len() >= MAX_ASKED && !l.r.contains_key(&k),
                    ),
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
                self.asked_list(&who);
                self.buddies.ask(&me, k.clone(), who.clone(), now.floor());
                self.touch_lists();
                (self.log)(&format!("[online] \"{bp}\" asked \"{who}\" to be friends"), Some(&group));
                let Some(o) = self.buddies.online.get(&k).copied() else { return Ok(()) };
                // to the asked one: the asker, as a request that waits for its answer (R: the player's menu then
                // offers to answer it, Accept, Decline or Block, 0x4D5190/0x4A1B50), then the asker's presence for
                // that entry
                self.bsend(o, "RNOT", &fields![("USER", bp.as_str()), ("CHNG", "A"), ("ATTR", ASKED_ME)], "")?;
                let p = presence(&self.buddies.conns[&b]);
                self.bsend(o, "PGET", &p, "")
            }
            "RRSP" => {
                // the answer to a friend request: Y accept, N decline, B block
                let who = strip_at(f.str("USER")).to_string();
                let k = lower(&who);
                let now = self.bnow();
                self.buddy_fetch(&k);
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
                // a message that reads as a slur, or one while the sender's are off, reaches no one (`msg_slur`)
                if kind == "SEND" && (self.msg_muted(&me) || self.msg_slur(&me, &bp, &clean_text(f.str("BODY"), 500), Some(&group))) {
                    return Ok(());
                }
                // away: a message from a friend goes to its pages, or waits for the persona (an invite is forgotten)
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

    // buddies.json of before read leniently and moved onto the shelf, one file a list, each request on both lists
    #[test]
    fn buddies_json_moved_onto_the_shelf() {
        let friends: Vec<String> = (0..60).map(|i| format!("\"Friend{i}\"")).collect();
        let many = format!(r#"{{"personas":[{{"name":"Many","seen":"2026-03","B":[{}]}}]}}"#, friends.join(","));
        let fifty = format!(r#"{{"v":4,"name":"Many","seen":"2026-03","B":[{}]}}"#, friends[..50].join(","));
        let mail: Vec<String> = (0..25).map(|i| format!(r#"{{"from":"Friend1","t":{i},"body":"m{i}","subj":"","type":"C"}}"#)).collect();
        let lots = format!(r#"{{"v":3,"personas":[{{"name":"Inbox","seen":"2026-10","B":["Friend1"],"M":[{}]}}]}}"#, mail.join(","));
        let twenty = format!(r#"{{"v":4,"name":"Inbox","seen":"2026-10","B":["Friend1"],"M":[{}]}}"#, mail[5..].join(","));
        let cases: Vec<(&str, Vec<&str>)> = vec![
            ("{not json", vec![]),
            ("", vec![]),
            (
                r#"{"v":2,"note":"NFSU2 online friends lists (persona names only)","personas":[{"name":"Alpha","seen":"2026-10","B":["Bravo","Charlie"]}]}"#,
                vec![r#"{"v":4,"name":"Alpha","seen":"2026-10","B":["Bravo","Charlie"]}"#],
            ),
            // a friend twice: the later spelling in the earlier one's place; friends that are no name; an owner twice:
            // the more recently seen list, in the other one's place
            (
                r#"{"personas":[{"name":"Alpha","seen":"2026-10","B":["Bravo","bravo","x y",5,null,"Delta"],"I":["Mallory"]},{"name":"alpha","seen":"2026-09","B":["Echo"]}]}"#,
                vec![r#"{"v":4,"name":"Alpha","seen":"2026-10","B":["bravo","Delta"],"I":["Mallory"]}"#],
            ),
            // owners that are no name not read; of the others the lists with anything in them, all of them
            (
                r#"{"personas":[{"name":"NoFriends","seen":"2026-10","B":[]},{"name":"Bad Name","B":["Alpha"]},{"name":"Old","B":["Alpha"]},{"name":"Mid","seen":"2026-05","B":["Alpha"]},{"name":"New","seen":"2026-12","B":"Alpha"},{"name":"Newer","seen":"2027-01","B":["Zed"]}]}"#,
                vec![
                    r#"{"v":4,"name":"Mid","seen":"2026-05","B":["Alpha"]}"#,
                    r#"{"v":4,"name":"Newer","seen":"2027-01","B":["Zed"]}"#,
                    r#"{"v":4,"name":"Old","seen":"1970-01","B":["Alpha"]}"#,
                ],
            ),
            // 50 friends at most
            (many.as_str(), vec![fifty.as_str()]),
            // version 3: the blocked, the requests sent (ones that are no name or no time left out), each also on the
            // asked one's list (one made for Charlie), the messages waiting (text cleaned, ones from no name or
            // without a time left out); a list with only some of them
            (
                r#"{"v":3,"personas":[{"name":"Alpha","seen":"2026-10","B":["Bravo"],"I":["Mallory","x y"],"R":[["Charlie",1761200000000],["x y",1],["Delta"],["Echo",-5]],"M":[{"from":"Bravo","t":1761200000123.7,"body":"hi\u0007 there","subj":"","type":"C"},{"from":"x y","t":1,"body":"no"},{"from":"Bravo","body":"no time"}]},{"name":"Lonely","seen":"2026-10","B":[],"R":[["Alpha",5]]}]}"#,
                vec![
                    r#"{"v":4,"name":"Alpha","seen":"2026-10","B":["Bravo"],"I":["Mallory"],"R":[["Charlie",1761200000000]],"Q":[["Lonely",5]],"M":[{"from":"Bravo","t":1761200000123,"body":"hi there","subj":"","type":"C"}]}"#,
                    r#"{"v":4,"name":"Charlie","seen":"1970-01","B":[],"Q":[["Alpha",1761200000000]]}"#,
                    r#"{"v":4,"name":"Lonely","seen":"2026-10","B":[],"R":[["Alpha",5]]}"#,
                ],
            ),
            // the newest 20 messages
            (lots.as_str(), vec![twenty.as_str()]),
        ];
        for (text, kept) in cases {
            let dir = tempfile::tempdir().unwrap();
            std::fs::write(dir.path().join(OLD_FILE), text).unwrap();
            let _ = Buddies::new(Some(dir.path().to_path_buf()), BuddyLimits::default());
            assert_eq!(shelf_texts(dir.path()), kept, "{text}");
            assert!(!dir.path().join(OLD_FILE).exists() && dir.path().join("buddies.json.moved").exists(), "{text}");
        }
    }

    // the shelf at scale (cargo test -p nfsu2-online --lib shelf_at_scale -- --ignored --nocapture): 200,000
    // players' lists of 5 friends each on disk, then players logging in (their lists read, one change each written)
    // with 20,000 lists in memory; what a save costs is the lists that changed, never the shelf's size
    #[test]
    #[ignore]
    fn shelf_at_scale() {
        use std::time::Instant;
        let dir = tempfile::tempdir().unwrap();
        let n = 200_000;
        let name = |i: usize| format!("P{i:06}");
        let t = Instant::now();
        for i in 0..n {
            let mut l = FriendsList::new(&name(i), "2026-10".into());
            for f in 1..=5 {
                let x = name((i + f * 7919) % n);
                l.b.insert(lower(&x), x);
            }
            Shelf::write(dir.path(), &lower(&name(i)), &list_json(&l)).unwrap();
        }
        let wrote = t.elapsed();
        let mut s = Buddies::new(Some(dir.path().to_path_buf()), BuddyLimits { lists: 20_000.0, ..Default::default() });
        let none = |_: &str| false;
        let t = Instant::now();
        let logins = 50_000;
        for k in 0..logins {
            let key = lower(&name((k * 104_729) % n));
            assert!(s.fetch(&key, 0.0, &none));
            s.mark(&key);
            if k % 1000 == 999 {
                assert!(s.write_dirty().is_none());
            }
        }
        let fetched = t.elapsed();
        eprintln!(
            "{n} lists written in {:.1} s ({:.0} us each); {logins} logins (a list read, then written again a thousand at a time) in {:.1} s ({:.0} us each); {} lists in memory",
            wrote.as_secs_f64(),
            wrote.as_secs_f64() * 1e6 / n as f64,
            fetched.as_secs_f64(),
            fetched.as_secs_f64() * 1e6 / logins as f64,
            s.lists.len()
        );
        assert!(s.lists.len() <= 20_000);
    }

    // a list read from the shelf the next time it is needed, as it was written; lists past `lists` go from memory
    // (written first) and come back from the shelf; the sweep takes the lists of claims that ended
    #[test]
    fn lists_come_and_go_from_memory() {
        let dir = tempfile::tempdir().unwrap();
        let claims = Arc::new(nfsu2_claims::Claims::in_memory());
        let (a, b) = (nfsu2_claims::owner_of_token(&"A".repeat(43)).unwrap(), nfsu2_claims::owner_of_token(&"B".repeat(43)).unwrap());
        claims.claim("Alpha", &a, 0).unwrap();
        claims.claim("Bravo", &b, 0).unwrap();
        let l = BuddyLimits { lists: 2.0, ..Default::default() };
        let mut s = Buddies::with_claims(Some(dir.path().to_path_buf()), l, Some(claims.clone()), 0);
        let none = |_: &str| false;
        for (n, o) in [("Alpha", &a), ("Bravo", &b)] {
            let mut x = FriendsList::new(n, "2026-10".into());
            x.owner = Some(o.clone());
            s.put_list(lower(n), x);
        }
        s.add_friend("alpha", "bravo".into(), "Bravo".into());
        s.add_friend("bravo", "alpha".into(), "Alpha".into());
        s.ask("alpha", "charlie".into(), "Charlie".into(), 7.0);
        assert!(s.write_dirty().is_none());
        // a third list pushes the least recently seen out of memory
        s.make_room(&none);
        s.lists.insert("charlie".into(), FriendsList::new("Charlie", "2026-10".into()));
        assert!(!s.lists.contains_key("alpha") && s.fans.get("bravo").is_none(), "Alpha's list out, and its fan entry");
        assert!(s.fetch("alpha", 0.0, &none), "back from the shelf");
        let x = s.list("Alpha").unwrap();
        assert_eq!((x.b.keys().map(String::as_str).collect::<Vec<_>>(), x.r.len(), x.owner.as_deref()), (vec!["bravo"], 1, Some(&*a)));
        // (Bravo's went for it) a list the caller pins is never pushed out: Charlie's goes for Bravo's
        assert!(!s.lists.contains_key("bravo"));
        assert!(s.fetch("bravo", 0.0, &|k: &str| k == "alpha"));
        assert!(s.lists.contains_key("alpha") && !s.lists.contains_key("charlie"));
        // a claim that ended: its list goes from the shelf when it is read, or when the shelf is swept
        claims.release(&b);
        let gone = sweep(dir.path(), &claims, 0, &HashSet::new());
        assert_eq!(gone, 1);
        assert!(Shelf::read(dir.path(), "bravo").is_none() && Shelf::read(dir.path(), "alpha").is_some());
    }
}
