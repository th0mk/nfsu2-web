//! The operator's bans (the server's control socket: `nfsu2-server ctl ban ...`): who may not play with others, or not
//! race them, for a while or for good, and the message their page shows them (nfsu2-lan's Kicks and bans).  The names
//! the operator took away (`rename`).  What each player had before, for a year ([`PAST_MS`]).  And the marks of the
//! players a `[cheat]` line of a race names: a handle (`c17`) that `ban` takes for [`MARK_MS`] (7 days), long after
//! the player left (`Bans::mark`).
//!
//! A ban is of a [`Kind`]: a full one turns the player's connections away (free roam, Online, LAN: the bans from
//! before kinds, and an entry of the file without one); a race ban lets the player play on with others but never race
//! them (the switch drops its LAN play and its free-roam lobby and outrun messages, the Online lobby refuses it a
//! game); a rename takes the player's name away from everyone (the name goes on the names taken away, `Bans::deny`,
//! and the player picks another) and is in force no longer than that moment: it is the player's history.
//!
//! There are no accounts, so a ban names a client the three ways the server knows one, each as a keyed hash
//! (HMAC-SHA256 under `bans.key` in the state folder, base64url, 22 characters): `bans.json` holds no address, no id
//! and no keyphrase's owner, and means nothing without the key.
//!
//! - Its address, as the server's limits count it (an IPv4 address, or an IPv6 /56), for at most [`ADDR_MS`] (7 days)
//!   after it was last the banned player's: addresses change hands (a phone network gives many players one address in
//!   turn, a home's address goes to another home), and an address banned for good would one day turn a stranger away.
//! - The id its page keeps in the browser's storage (the hello's "id", web/src/app/page-id.ts), for as long as the ban
//!   lasts.  A page that found another id in its other store names that one too (the hello's "was").
//! - The keyphrase its page plays with (the one that keeps its cloud saves and claims its name), by the owner
//!   nfsu2-claims makes of its token (the switch's `AttachOpts::on_claim`), for as long as the ban lasts: the same on
//!   every device and browser the player types it into.
//!
//! A connection that matches a full ban is turned away ([`Bans::by_addr`] when it connects, [`Bans::by_hello`] at
//! its hello); one that matches a race ban (at its hello, its address too) races nobody.  A hello that matches by what
//! the ban was made with (the player's page ids and keyphrase then) adds what else it names: its address (as above),
//! and its other page ids and keyphrase, each for [`LINK_MS`] (30 days) after it was last seen with one of those, at
//! most [`KEYS_MAX`] of each (the first ones stay).  So the player's new browser is turned away while it keeps showing
//! up with the banned keyphrase, and a month after.  A hello that matches only by what was added so is turned away
//! and adds nothing: a keyphrase the player typed into someone else's browser bans that browser for a month at most,
//! and never its owner's own keyphrase or address.  An address never adds anything (whoever shares it would be
//! banned).  None of it stops a player who clears the site's data, leaves its keyphrase (and its cloud saves and its
//! name) behind and changes networks: without accounts no ban can.
//!
//! A new ban takes the place of the bans of its kind that name one of the same page ids or keyphrases (for a player
//! without either: the same address), so `ban Bob perm` after `ban Bob 1d` leaves one ban, for good; a race ban and a
//! full ban of one player both stand (the full one ends, the race ban goes on).  A ban whose time is up, one lifted
//! and one replaced go to the player's history (`Bans::history`), kept [`PAST_MS`] after it ended; so do renames.
//! What is over goes at the next change or listing; so do marks.  The file is written whole: a temporary file beside
//! it (0600), renamed over it.  A server from before kinds reads a race ban as a full one (its file has no kind).

use std::io::{self, Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use hmac::{Hmac, Mac};
use parking_lot::Mutex;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use sha2::Sha256;

/// how long an address stays banned after it was last the banned player's (ms)
pub const ADDR_MS: i64 = 7 * DAY;
/// how long a page id or keyphrase a ban took in later stays banned after it was last seen with the ban's own (ms)
pub const LINK_MS: i64 = 30 * DAY;
/// how long a `[cheat]` line's mark lasts after its last line (ms)
pub const MARK_MS: i64 = 7 * DAY;
/// how long a ban that ended, and a rename, stay in the player's history (ms)
pub const PAST_MS: i64 = 365 * DAY;
/// the longest message a kick or a ban carries (characters)
pub const MSG_MAX: usize = 200;
/// the longest ban short of one for good (ms): ten years
pub const FOR_MAX: i64 = 3650 * DAY;
/// the most page ids, and keyphrases, a ban takes in later, or a mark names: past them a hello adds none (a player
/// who keeps clearing its storage, or a script making up ids, does not grow the file without end; the first stay)
pub const KEYS_MAX: usize = 32;
/// the most marks kept (the oldest go first)
const MARKS_MAX: usize = 5000;
/// the most bans kept in the history (the oldest go first), and names taken away
const PAST_MAX: usize = 20_000;
const NAMES_MAX: usize = 10_000;
/// the most names of earlier lines a mark keeps, and bans (the latest stay)
const MARK_NAMES_MAX: usize = 8;
/// a key seen again this soon after its time was last set is not written again
const SLACK: i64 = 3_600_000;
const MIN: i64 = 60_000;
const HOUR: i64 = 60 * MIN;
const DAY: i64 = 24 * HOUR;

/// What a ban takes away (the crate doc)
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    /// playing with others at all: its connections are turned away
    #[default]
    Full,
    /// racing others: Online's games, free roam's lobby races and outruns, LAN play
    Races,
    /// its name (`Bans::rename`): in force no longer than that moment
    Name,
}

impl Kind {
    /// for the operator: "ban", "race ban", "rename"
    pub fn word(self) -> &'static str {
        match self {
            Kind::Full => "ban",
            Kind::Races => "race ban",
            Kind::Name => "rename",
        }
    }
}

/// One ban.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Ban {
    /// its number (`unban #4`)
    pub id: u64,
    /// what it takes away (a file from before kinds: full bans)
    #[serde(default)]
    pub kind: Kind,
    /// the player's name when it was banned, for the operator (anyone can take any name: a ban never matches by one)
    pub name: String,
    /// what the player's page shows
    #[serde(default)]
    pub msg: String,
    /// when it was made, and until when it lasts (ms since 1970; None: for good)
    pub at: i64,
    pub until: Option<i64>,
    /// the hashes of the page ids and keyphrases (their owners: nfsu2-claims) it was made with, for as long as it lasts
    #[serde(default)]
    pub pages: Vec<String>,
    #[serde(default)]
    pub owners: Vec<String>,
    /// and of the ones it took in later (`Bans::by_hello`), each until its own time
    #[serde(default)]
    pub later_pages: Vec<Held>,
    #[serde(default)]
    pub later_owners: Vec<Held>,
    /// and of its addresses, each until its own time
    #[serde(default)]
    pub addrs: Vec<Held>,
}

/// A key a ban names until its own time, never past the ban's end: an address (`ADDR_MS` after it was last seen as
/// the player's), or a page id or keyphrase it took in later (`LINK_MS` after it was last seen with the ban's own)
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Held {
    /// its hash
    pub h: String,
    /// until when (ms since 1970)
    pub until: i64,
}

impl Ban {
    /// whether it is in force now (a rename never is)
    pub fn live(&self, now: i64) -> bool {
        self.kind != Kind::Name && self.until.is_none_or(|u| u > now)
    }
    /// Its page's control message: {"t":"ban","msg":...,"until":ms} for a full ban, {"t":"races",...} for a race
    /// ban, without until for good
    pub fn message(&self) -> String {
        let mut m = Map::new();
        m.insert("t".into(), if self.kind == Kind::Races { "races" } else { "ban" }.into());
        m.insert("msg".into(), self.msg.clone().into());
        if let Some(u) = self.until {
            m.insert("until".into(), u.into());
        }
        Value::Object(m).to_string()
    }
    /// whether it was made with one of these page ids or keyphrases (their hashes)
    fn made_for(&self, pages: &[String], owners: &[String]) -> bool {
        pages.iter().any(|p| self.pages.contains(p)) || owners.iter().any(|o| self.owners.contains(o))
    }
    /// whether it took in one of them later, and holds it now
    fn took_in(&self, pages: &[String], owners: &[String], now: i64) -> bool {
        let held = |l: &[Held], h: &String| l.iter().any(|x| &x.h == h && x.until > now);
        pages.iter().any(|p| held(&self.later_pages, p)) || owners.iter().any(|o| held(&self.later_owners, o))
    }
    /// whether it names this address (its hash) now
    fn at_addr(&self, a: &str, now: i64) -> bool {
        self.addrs.iter().any(|x| x.h == a && x.until > now)
    }
    /// until when a key it holds for `ms` from now lasts: never past its own end
    fn until_for(&self, ms: i64, now: i64) -> i64 {
        self.until.map_or(now + ms, |u| u.min(now + ms))
    }
    /// every key it named, whatever their time (the player's history)
    pub fn keys(&self) -> Keys {
        let hs = |l: &[Held]| l.iter().map(|x| x.h.clone()).collect::<Vec<_>>();
        Keys {
            addrs: hs(&self.addrs),
            pages: self.pages.iter().cloned().chain(hs(&self.later_pages)).collect(),
            owners: self.owners.iter().cloned().chain(hs(&self.later_owners)).collect(),
        }
    }
}

/// How a ban in the history ended (`Past`)
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum How {
    /// its time was up (a rename: made)
    Over,
    /// the operator lifted it (`unban`)
    Lifted,
    /// a new ban of its kind took its place (`Past::by`)
    Replaced,
}

/// A ban that ended, or a rename, in its player's history for `PAST_MS` after `ended`
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Past {
    pub ban: Ban,
    /// when it ended (ms since 1970), and how
    pub ended: i64,
    pub how: How,
    /// the ban that took its place
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub by: Option<u64>,
}

/// One of a player's bans (`Bans::history`): in force, or in the history
#[derive(Clone, Debug, PartialEq)]
pub struct Record {
    pub ban: Ban,
    /// None: in force
    pub past: Option<(i64, How, Option<u64>)>,
}

/// A name the operator took away (`Bans::rename`), from everyone, for good (`unname` gives it back)
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Denied {
    /// as the player had it
    pub name: String,
    pub at: i64,
    /// the rename's number
    pub by: u64,
}

/// What a hello's player is under (`Bans::by_hello`): the full ban that turns it away, the race ban that keeps it
/// out of races (each the one that lasts longest)
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Verdict {
    pub full: Option<Ban>,
    pub races: Option<Ban>,
}

/// Whom a ban or a mark names: the keyed hashes of addresses (`Bans::addr`), page ids and keyphrases (`Bans::keys`)
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Keys {
    #[serde(default)]
    pub addrs: Vec<String>,
    #[serde(default)]
    pub pages: Vec<String>,
    #[serde(default)]
    pub owners: Vec<String>,
}

impl Keys {
    pub fn is_empty(&self) -> bool {
        self.addrs.is_empty() && self.pages.is_empty() && self.owners.is_empty()
    }
    /// another's added, each once, at most `KEYS_MAX` of each
    pub fn extend(&mut self, o: Keys) {
        for (to, from) in [(&mut self.addrs, o.addrs), (&mut self.pages, o.pages), (&mut self.owners, o.owners)] {
            for h in from {
                if !to.contains(&h) && to.len() < KEYS_MAX {
                    to.push(h);
                }
            }
        }
    }
    /// whether they name one of the same page ids or keyphrases (for keys without either: one of the same addresses)
    pub fn same_player(&self, o: &Keys) -> bool {
        if self.pages.is_empty() && self.owners.is_empty() {
            return self.addrs.iter().any(|a| o.addrs.contains(a));
        }
        self.pages.iter().any(|p| o.pages.contains(p)) || self.owners.iter().any(|x| o.owners.contains(x))
    }
}

/// a kick's control message: {"t":"kick","msg":...}
pub fn kick_message(msg: &str) -> String {
    let mut m = Map::new();
    m.insert("t".into(), "kick".into());
    m.insert("msg".into(), clean_msg(msg).into());
    Value::Object(m).to_string()
}

/// a rename's control message, which the page shows before it asks for another name: {"t":"rename","name":...,"msg":...}
pub fn rename_message(name: &str, msg: &str) -> String {
    let mut m = Map::new();
    m.insert("t".into(), "rename".into());
    m.insert("name".into(), clean_msg(name).into());
    m.insert("msg".into(), clean_msg(msg).into());
    Value::Object(m).to_string()
}

/// a race ban lifted while its player plays: {"t":"races","lifted":true}
pub fn races_lifted_message() -> String {
    r#"{"t":"races","lifted":true}"#.to_string()
}

/// What a hello names its client by (`Bans::by_hello`): its page's ids (the one it plays with, then the ones the page
/// found besides it), the owner of the keyphrase it plays with (nfsu2-claims), its address's hash (`Bans::addr`)
pub struct Seen<'a> {
    pub pages: &'a [&'a str],
    pub owner: Option<&'a str>,
    pub addr: Option<&'a str>,
}

/// A `[cheat]` line's mark (`Bans::mark`): the player the line named, for `ban c<n>`
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Mark {
    pub n: u64,
    /// the player's name at its last line, and when that was (ms since 1970)
    pub name: String,
    pub at: i64,
    pub keys: Keys,
    /// the other names its earlier lines gave it (each once, oldest first)
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub before: Vec<String>,
    /// the bans made of the player since its first line (`Bans::add`: by the handle, or by its name or address)
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub bans: Vec<u64>,
}

/// What a `[cheat]` line of a race says of its player (`Bans::mark`): its handle, the other names of its earlier lines
/// (oldest first), and the bans made of it since its first line, each with whether it is in force and its kind
#[derive(Clone, Debug, PartialEq)]
pub struct Marked {
    pub handle: String,
    pub before: Vec<String>,
    pub bans: Vec<(u64, bool, Kind)>,
}

impl Mark {
    /// its handle: "c17"
    pub fn handle(&self) -> String {
        format!("c{}", self.n)
    }
}

#[derive(Default, Serialize, Deserialize)]
struct File {
    /// the last ban's number (renames count too)
    next: u64,
    bans: Vec<Ban>,
    /// the last mark's number, and the marks
    #[serde(default)]
    next_mark: u64,
    #[serde(default)]
    marks: Vec<Mark>,
    /// the bans that ended and the renames, oldest first
    #[serde(default)]
    past: Vec<Past>,
    /// the names taken away, oldest first
    #[serde(default)]
    names: Vec<Denied>,
}

/// The bans of a state folder.  Every call takes one lock; the ones that change something write the file under it
/// (rare: the operator's commands, a banned player in a new browser, with a new keyphrase or at a new address, and a
/// `[cheat]` line of a race).
pub struct Bans {
    path: PathBuf,
    key: Vec<u8>,
    file: Mutex<File>,
    deny: Arc<nfsu2_names::Deny>,
}

impl Bans {
    /// The bans in `dir` (bans.json, and its key bans.key, made at the first start).  Err: a file that cannot be read.
    pub fn open(dir: &Path) -> io::Result<Bans> {
        let key = load_key(&dir.join("bans.key"))?;
        let path = dir.join("bans.json");
        let file: File = match std::fs::read(&path) {
            Ok(b) => serde_json::from_slice(&b).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?,
            Err(e) if e.kind() == io::ErrorKind::NotFound => File::default(),
            Err(e) => return Err(e),
        };
        let deny = Arc::new(nfsu2_names::Deny::default());
        deny.set(file.names.iter().map(|d| d.name.clone()));
        Ok(Bans { path, key, file: Mutex::new(file), deny })
    }

    /// the names taken away, as the switch, the Online lobby and free roam's hub look them up (it follows every change)
    pub fn deny(&self) -> Arc<nfsu2_names::Deny> {
        self.deny.clone()
    }

    /// an address's hash: what bans name it by, and what the switch keeps of it (`AttachOpts::ident`)
    pub fn addr(&self, key: &str) -> String {
        self.hash("addr", key)
    }

    /// a player's keys as the switch knows them: its address's hash (`Bans::addr`), its page's id and its keyphrase's
    /// owner as they came
    pub fn keys(&self, addr: Option<&str>, page: Option<&str>, owner: Option<&str>) -> Keys {
        Keys {
            addrs: addr.map(str::to_string).into_iter().collect(),
            pages: page.map(|p| self.page(p)).into_iter().collect(),
            owners: owner.map(|o| self.owner(o)).into_iter().collect(),
        }
    }

    fn page(&self, id: &str) -> String {
        self.hash("page", id)
    }

    fn owner(&self, owner: &str) -> String {
        self.hash("owner", owner)
    }

    fn hash(&self, kind: &str, v: &str) -> String {
        let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(&self.key).expect("HMAC takes any key");
        mac.update(kind.as_bytes());
        mac.update(b"\0");
        mac.update(v.as_bytes());
        nfsu2_util::js::base64url(&mac.finalize().into_bytes())[..22].to_string()
    }

    /// the live full ban an address (its hash) is under now, the one that lasts longest
    pub fn by_addr(&self, addr: &str, now: i64) -> Option<Ban> {
        let f = self.file.lock();
        longest(&f.bans, |b| b.kind == Kind::Full && b.live(now) && b.at_addr(addr, now)).map(|i| f.bans[i].clone())
    }

    /// The live bans a hello's page ids or keyphrase are under now (a race ban: its address too), of each kind the
    /// one that lasts longest.  When one was made with them, what else the hello names is added to it: its other page
    /// ids and its keyphrase for `LINK_MS` (up to `KEYS_MAX` each), the address it came from (its hash) for
    /// `ADDR_MS`, or each kept that much longer.  A hello that matches only by what a ban took in later (or by its
    /// address) adds nothing.
    pub fn by_hello(&self, s: &Seen, now: i64) -> Verdict {
        let pages: Vec<String> = s.pages.iter().map(|p| self.page(p)).collect();
        let owners: Vec<String> = s.owner.map(|o| self.owner(o)).into_iter().collect();
        let mut f = self.file.lock();
        let (mut v, mut changed) = (Verdict::default(), false);
        for kind in [Kind::Full, Kind::Races] {
            let ban = match longest(&f.bans, |b| b.kind == kind && b.live(now) && b.made_for(&pages, &owners)) {
                Some(i) => {
                    let b = &mut f.bans[i];
                    let (link, addr) = (b.until_for(LINK_MS, now), b.until_for(ADDR_MS, now));
                    for p in pages.iter().filter(|p| !b.pages.contains(p)) {
                        changed |= hold(&mut b.later_pages, p, link, KEYS_MAX);
                    }
                    for o in owners.iter().filter(|o| !b.owners.contains(o)) {
                        changed |= hold(&mut b.later_owners, o, link, KEYS_MAX);
                    }
                    if let Some(a) = s.addr {
                        changed |= hold(&mut b.addrs, a, addr, usize::MAX);
                    }
                    Some(b.clone())
                }
                None => longest(&f.bans, |b| {
                    b.kind == kind
                        && b.live(now)
                        && (b.took_in(&pages, &owners, now) || (kind == Kind::Races && s.addr.is_some_and(|a| b.at_addr(a, now))))
                })
                .map(|i| f.bans[i].clone()),
            };
            match kind {
                Kind::Full => v.full = ban,
                _ => v.races = ban,
            }
        }
        if changed {
            self.save_or_say(&mut f, now);
        }
        v
    }

    /// whether `ban` names one of these keys (a connection's: `Bans::keys`) now: the other connections a new ban removes
    pub fn covers(&self, ban: &Ban, k: &Keys, now: i64) -> bool {
        ban.made_for(&k.pages, &k.owners) || ban.took_in(&k.pages, &k.owners, now) || k.addrs.iter().any(|a| ban.at_addr(a, now))
    }

    /// A new ban of `kind` (full or races) of the player `k` names (`name` for the operator), with `msg`, until
    /// `until` (None: for good).  It takes the place of the bans of its kind that name one of its page ids or
    /// keyphrases (a player without either: one of its addresses); their numbers come back with it.
    pub fn add(&self, kind: Kind, name: &str, k: &Keys, msg: &str, until: Option<i64>, now: i64) -> io::Result<(Ban, Vec<u64>)> {
        if kind == Kind::Name {
            return Err(io::Error::other("a rename is no ban to add (Bans::rename)"));
        }
        let mut f = self.file.lock();
        prune(&mut f, now);
        let id = f.next + 1;
        let by_id = !k.pages.is_empty() || !k.owners.is_empty();
        let (same, rest): (Vec<Ban>, Vec<Ban>) = std::mem::take(&mut f.bans).into_iter().partition(|b| {
            b.kind == kind
                && if by_id {
                    b.made_for(&k.pages, &k.owners) || b.took_in(&k.pages, &k.owners, now)
                } else {
                    k.addrs.iter().any(|a| b.addrs.iter().any(|x| &x.h == a))
                }
        });
        f.bans = rest;
        let replaced: Vec<u64> = same.iter().map(|b| b.id).collect();
        f.past.extend(same.into_iter().map(|ban| Past { ban, ended: now, how: How::Replaced, by: Some(id) }));
        f.next = id;
        let b = new_ban(id, kind, name, k, msg, until, now);
        f.bans.push(b.clone());
        note_on_marks(&mut f, k, id, now);
        self.save(&mut f, now)?;
        Ok((b, replaced))
    }

    /// The player `k` names (`player` for the operator) has its `names` taken away from everyone, with `msg`: a
    /// rename in its history, and the names on the names taken away (`deny`: as they read, nfsu2-names `letters`), but
    /// for one with no letters, NAME and Player (the game's names for nobody in particular) and one taken away
    /// already.  The rename comes back with the names it took away.
    pub fn rename(&self, player: &str, names: &[String], k: &Keys, msg: &str, now: i64) -> io::Result<(Ban, Vec<String>)> {
        let mut f = self.file.lock();
        prune(&mut f, now);
        f.next += 1;
        let id = f.next;
        let b = new_ban(id, Kind::Name, player, k, msg, Some(now), now);
        f.past.push(Past { ban: b.clone(), ended: now, how: How::Over, by: None });
        let nobody = [nfsu2_names::letters("NAME"), nfsu2_names::letters("Player")];
        let mut took = Vec::new();
        for n in names {
            let l = nfsu2_names::letters(n);
            if l.is_empty() || nobody.contains(&l) || f.names.iter().any(|d| nfsu2_names::letters(&d.name) == l) {
                continue;
            }
            let name: String = clean_msg(n).chars().take(64).collect();
            f.names.push(Denied { name: name.clone(), at: now, by: id });
            took.push(name);
        }
        let over = f.names.len().saturating_sub(NAMES_MAX);
        f.names.drain(..over);
        self.deny.set(f.names.iter().map(|d| d.name.clone()));
        note_on_marks(&mut f, k, id, now);
        self.save(&mut f, now)?;
        Ok((b, took))
    }

    /// the name taken away that `name` reads as, if any
    pub fn denied_as(&self, name: &str) -> Option<Denied> {
        let l = nfsu2_names::letters(name);
        if l.is_empty() {
            return None;
        }
        self.file.lock().names.iter().find(|d| nfsu2_names::letters(&d.name) == l).cloned()
    }

    /// the names taken away, oldest first
    pub fn denied(&self) -> Vec<Denied> {
        self.file.lock().names.clone()
    }

    /// The names taken away that `name` reads as, given back
    pub fn undeny(&self, name: &str, now: i64) -> io::Result<Vec<Denied>> {
        let l = nfsu2_names::letters(name);
        let mut f = self.file.lock();
        let (gone, kept): (Vec<Denied>, Vec<Denied>) = std::mem::take(&mut f.names)
            .into_iter()
            .partition(|d| d.name.eq_ignore_ascii_case(name.trim()) || (!l.is_empty() && nfsu2_names::letters(&d.name) == l));
        f.names = kept;
        if !gone.is_empty() {
            self.deny.set(f.names.iter().map(|d| d.name.clone()));
            self.save(&mut f, now)?;
        }
        Ok(gone)
    }

    /// The bans `sel` names, lifted (to their players' history): "#4" or "4" (a number), or a name (any case: every
    /// ban of that name).
    pub fn remove(&self, sel: &str, now: i64) -> io::Result<Vec<Ban>> {
        let sel = sel.trim();
        let num = sel.strip_prefix('#').unwrap_or(sel).parse::<u64>().ok();
        let mut f = self.file.lock();
        prune(&mut f, now);
        let (gone, kept): (Vec<Ban>, Vec<Ban>) = std::mem::take(&mut f.bans).into_iter().partition(|b| match num {
            Some(n) => b.id == n,
            None => b.name.to_lowercase() == sel.to_lowercase(),
        });
        f.bans = kept;
        if !gone.is_empty() {
            f.past.extend(gone.iter().map(|b| Past { ban: b.clone(), ended: now, how: How::Lifted, by: None }));
            self.save(&mut f, now)?;
        }
        Ok(gone)
    }

    /// The live ban #`id` made another kind (full or races) and/or to last `until` (None: for good): it before and
    /// after.  None: no such ban in force.
    pub fn change(&self, id: u64, kind: Option<Kind>, until: Option<Option<i64>>, now: i64) -> io::Result<Option<(Ban, Ban)>> {
        let mut f = self.file.lock();
        prune(&mut f, now);
        let Some(b) = f.bans.iter_mut().find(|b| b.id == id) else { return Ok(None) };
        let before = b.clone();
        if let Some(k) = kind.filter(|&k| k != Kind::Name) {
            b.kind = k;
        }
        if let Some(u) = until {
            b.until = u;
            // what it holds for a time never outlasts it
            if let Some(end) = u {
                for l in [&mut b.addrs, &mut b.later_pages, &mut b.later_owners] {
                    l.iter_mut().for_each(|x| x.until = x.until.min(end));
                }
            }
        }
        let after = b.clone();
        self.save(&mut f, now)?;
        Ok(Some((before, after)))
    }

    /// the live bans, oldest first (the ones whose time is up go)
    pub fn list(&self, now: i64) -> Vec<Ban> {
        let mut f = self.file.lock();
        let before = counts(&f);
        prune(&mut f, now);
        if before != counts(&f) {
            self.save_or_say(&mut f, now);
        }
        f.bans.clone()
    }

    /// The ban (in force, or in the history) of that number
    pub fn record(&self, id: u64, now: i64) -> Option<Record> {
        let mut f = self.file.lock();
        prune(&mut f, now);
        f.bans
            .iter()
            .find(|b| b.id == id)
            .map(|b| Record { ban: b.clone(), past: None })
            .or_else(|| f.past.iter().find(|p| p.ban.id == id).map(past_record))
    }

    /// The bans of the player `k` names, in force and in the history (renames too), oldest first: each one that
    /// named one of its page ids or keyphrases (for keys without either: one of its addresses), whatever their time
    pub fn history(&self, k: &Keys, now: i64) -> Vec<Record> {
        let mut f = self.file.lock();
        prune(&mut f, now);
        let mut v: Vec<Record> = f.past.iter().filter(|p| k.same_player(&p.ban.keys())).map(past_record).collect();
        v.extend(f.bans.iter().filter(|b| k.same_player(&b.keys())).map(|b| Record { ban: b.clone(), past: None }));
        v.sort_by_key(|r| (r.ban.at, r.ban.id));
        v
    }

    /// The bans (in force and in the history) of a player called `name` (any case), oldest first: for one the switch
    /// does not know now
    pub fn history_by_name(&self, name: &str, now: i64) -> Vec<Record> {
        let mut f = self.file.lock();
        prune(&mut f, now);
        let is = |b: &Ban| b.name.eq_ignore_ascii_case(name.trim());
        let mut v: Vec<Record> = f.past.iter().filter(|p| is(&p.ban)).map(past_record).collect();
        v.extend(f.bans.iter().filter(|b| is(b)).map(|b| Record { ban: b.clone(), past: None }));
        v.sort_by_key(|r| (r.ban.at, r.ban.id));
        v
    }

    /// The handle of a `[cheat]` line's player (`k`, `name` for the operator): "c17", which `ban` takes for `MARK_MS`;
    /// with the other names of its earlier lines and the bans made of it since.  The same player again (one of the
    /// same page ids or keyphrases; without either, the same address) keeps its handle, and its time starts again.
    /// The file is written for a new mark, a new name or key of one, or a time SLACK on: many lines of one player at
    /// once (a race's, a cheat's every race) write it once.
    pub fn mark(&self, name: &str, k: Keys, now: i64) -> io::Result<Marked> {
        let mut f = self.file.lock();
        let (m, changed) = match f.marks.iter_mut().find(|m| m.at + MARK_MS > now && k.same_player(&m.keys)) {
            Some(m) => {
                let count = |k: &Keys| k.addrs.len() + k.pages.len() + k.owners.len();
                let keys = count(&m.keys);
                m.keys.extend(k);
                let changed = m.name != name || now - m.at >= SLACK || count(&m.keys) != keys;
                if m.name != name {
                    let was = std::mem::replace(&mut m.name, name.to_string());
                    let same = |a: &str, b: &str| a.to_lowercase() == b.to_lowercase();
                    m.before.retain(|x| !same(x, name) && !same(x, &was));
                    if !same(&was, name) {
                        m.before.push(was);
                    }
                    if m.before.len() > MARK_NAMES_MAX {
                        m.before.remove(0);
                    }
                }
                m.at = now;
                (m.clone(), changed)
            }
            None => {
                f.next_mark += 1;
                let mut keys = Keys::default();
                keys.extend(k);
                let m = Mark { n: f.next_mark, name: name.to_string(), at: now, keys, before: vec![], bans: vec![] };
                f.marks.push(m.clone());
                if f.marks.len() > MARKS_MAX {
                    f.marks.remove(0);
                }
                (m, true)
            }
        };
        let of = |id: u64| {
            let b = f.bans.iter().find(|b| b.id == id).or_else(|| f.past.iter().map(|p| &p.ban).find(|b| b.id == id));
            b.map_or((id, false, Kind::Full), |b| (id, b.live(now), b.kind))
        };
        let bans = m.bans.iter().map(|&id| of(id)).collect();
        if changed {
            self.save(&mut f, now)?;
        }
        Ok(Marked { handle: m.handle(), before: m.before, bans })
    }

    /// the live mark of a handle ("c17", any case)
    pub fn marked(&self, handle: &str, now: i64) -> Option<Mark> {
        let n = handle.trim().strip_prefix(['c', 'C'])?.parse::<u64>().ok()?;
        self.file.lock().marks.iter().find(|m| m.n == n && m.at + MARK_MS > now).cloned()
    }

    fn save_or_say(&self, f: &mut File, now: i64) {
        if let Err(e) = self.save(f, now) {
            nfsu2_util::log::error(&format!("{}: {e}", self.path.display()));
        }
    }

    /// the file written whole, without what is over
    fn save(&self, f: &mut File, now: i64) -> io::Result<()> {
        prune(f, now);
        let text = serde_json::to_string_pretty(&*f).map_err(io::Error::other)?;
        let tmp = self.path.with_extension(format!("json.tmp-{}", std::process::id()));
        let mut out = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
        out.write_all(text.as_bytes())?;
        out.write_all(b"\n")?;
        out.sync_all()?;
        std::fs::rename(&tmp, &self.path)
    }
}

/// a ban as `add` and `rename` make it: the keys it was made with, its addresses for `ADDR_MS`
fn new_ban(id: u64, kind: Kind, name: &str, k: &Keys, msg: &str, until: Option<i64>, now: i64) -> Ban {
    let mut b = Ban {
        id,
        kind,
        name: name.to_string(),
        msg: clean_msg(msg),
        at: now,
        until,
        pages: vec![],
        owners: vec![],
        later_pages: vec![],
        later_owners: vec![],
        addrs: vec![],
    };
    let mut own = Keys::default();
    own.extend(k.clone());
    let addr = if kind == Kind::Name { now } else { b.until_for(ADDR_MS, now) };
    b.addrs = own.addrs.into_iter().map(|h| Held { h, until: addr }).collect();
    (b.pages, b.owners) = (own.pages, own.owners);
    b
}

/// the marks of the player `k` names (a ban by its handle, or of one with a mark by its name or address) have ban `id`
fn note_on_marks(f: &mut File, k: &Keys, id: u64, now: i64) {
    for m in f.marks.iter_mut().filter(|m| m.at + MARK_MS > now && k.same_player(&m.keys)) {
        m.bans.push(id);
        if m.bans.len() > MARK_NAMES_MAX {
            m.bans.remove(0);
        }
    }
}

fn past_record(p: &Past) -> Record {
    Record { ban: p.ban.clone(), past: Some((p.ended, p.how, p.by)) }
}

/// `h` held until `until` in `l` (at most `max` long): false when that changed nothing worth writing
fn hold(l: &mut Vec<Held>, h: &str, until: i64, max: usize) -> bool {
    if let Some(x) = l.iter_mut().find(|x| x.h == h) {
        let later = x.until < until - SLACK;
        if later {
            x.until = until;
        }
        return later;
    }
    if l.len() >= max {
        return false;
    }
    l.push(Held { h: h.to_string(), until });
    true
}

/// the bans whose time is up go to the history, what the others name for a time goes when its time is up, and so do
/// the history's bans a year after they ended and the marks whose time is up
fn prune(f: &mut File, now: i64) {
    let (live, over): (Vec<Ban>, Vec<Ban>) = std::mem::take(&mut f.bans).into_iter().partition(|b| b.live(now));
    f.bans = live;
    f.past.extend(over.into_iter().map(|ban| Past { ended: ban.until.unwrap_or(now), how: How::Over, by: None, ban }));
    for b in &mut f.bans {
        for l in [&mut b.addrs, &mut b.later_pages, &mut b.later_owners] {
            l.retain(|x| x.until > now);
        }
    }
    f.past.retain(|p| p.ended + PAST_MS > now);
    let over = f.past.len().saturating_sub(PAST_MAX);
    f.past.drain(..over);
    f.marks.retain(|m| m.at + MARK_MS > now);
}

/// how much the file holds: what `list` compares to see whether `prune` took anything
fn counts(f: &File) -> (usize, usize, usize, usize) {
    let held = f.bans.iter().map(|b| b.addrs.len() + b.later_pages.len() + b.later_owners.len()).sum();
    (f.bans.len(), held, f.marks.len(), f.past.len())
}

/// of the bans `f` takes, the index of the one that lasts longest (for good first)
fn longest(bans: &[Ban], f: impl Fn(&Ban) -> bool) -> Option<usize> {
    bans.iter().enumerate().filter(|(_, b)| f(b)).max_by_key(|(_, b)| b.until.unwrap_or(i64::MAX)).map(|(i, _)| i)
}

/// the key (32 random bytes), made 0600 at the first start
fn load_key(p: &Path) -> io::Result<Vec<u8>> {
    if let Ok(mut file) = std::fs::File::open(p) {
        let mut k = Vec::new();
        if file.read_to_end(&mut k).is_ok() && k.len() >= 32 {
            return Ok(k);
        }
    }
    let k = nfsu2_util::rand::bytes(32);
    let mut file = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(p)?;
    file.write_all(&k)?;
    Ok(k)
}

/// a message as the page shows it: one line, no control or direction characters, at most `MSG_MAX` characters
pub fn clean_msg(s: &str) -> String {
    let bidi = |c: char| matches!(c, '\u{200B}'..='\u{200F}' | '\u{202A}'..='\u{202E}' | '\u{2060}'..='\u{2069}' | '\u{FEFF}');
    let t: String = s.chars().map(|c| if c.is_whitespace() { ' ' } else { c }).filter(|&c| !c.is_control() && !bidi(c)).collect();
    t.split(' ').filter(|w| !w.is_empty()).collect::<Vec<_>>().join(" ").chars().take(MSG_MAX).collect()
}

/// How long a ban lasts, as the operator writes it: "30m", "12h", "3d", "2w" (a whole number and its unit; "min" for
/// m too), or "perm" (for good: None).  Not one of those (or over `FOR_MAX`): Err.
pub fn parse_for(s: &str) -> Result<Option<i64>, String> {
    let s = s.trim().to_lowercase();
    if matches!(s.as_str(), "perm" | "permanent" | "forever") {
        return Ok(None);
    }
    let at = s.find(|c: char| !c.is_ascii_digit()).unwrap_or(s.len());
    let (n, unit) = s.split_at(at);
    let unit_ms = match unit {
        "m" | "min" => MIN,
        "h" => HOUR,
        "d" => DAY,
        "w" => 7 * DAY,
        _ => return Err(format!("\"{s}\": how long, as 30m, 12h, 3d, 2w or perm")),
    };
    match n.parse::<i64>() {
        Ok(n) if n > 0 && n.checked_mul(unit_ms).is_some_and(|ms| ms <= FOR_MAX) => Ok(Some(n * unit_ms)),
        _ => Err(format!("\"{s}\": how long, as 30m, 12h, 3d, 2w or perm (at most ten years)")),
    }
}

/// a span of time, for the operator: "45 min", "5 h 10 min", "2 d 23 h"
pub fn span(ms: i64) -> String {
    let m = (ms.max(0) + MIN - 1) / MIN;
    let (d, h, mi) = (m / (24 * 60), m / 60 % 24, m % 60);
    match (d, h, mi) {
        (0, 0, mi) => format!("{mi} min"),
        (0, h, 0) => format!("{h} h"),
        (0, h, mi) => format!("{h} h {mi} min"),
        (d, 0, _) => format!("{d} d"),
        (d, h, _) => format!("{d} d {h} h"),
    }
}

/// a time, for the operator: "2026-10-27 14:05 UTC"
pub fn when(ms: i64) -> String {
    let (y, mo, d, h, mi, _, _) = nfsu2_util::js::utc_parts(ms);
    format!("{y:04}-{mo:02}-{d:02} {h:02}:{mi:02} UTC")
}

#[cfg(test)]
mod tests;
