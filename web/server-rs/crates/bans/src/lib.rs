//! The operator's bans (the server's control socket: `nfsu2-server ctl ban ...`): who may not play with others, for
//! a while or for good, and the message their page shows them (nfsu2-lan's Kicks and bans).  And the marks of the
//! players a `[cheat]` line of a race names: a handle (`c17`) that `ban` takes for [`MARK_MS`] (7 days), long after
//! the player left (`Bans::mark`).
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
//! A connection that matches any is turned away ([`Bans::by_addr`] when it connects, [`Bans::by_hello`] at its
//! hello).  A hello that matches by what the ban was made with (the player's page ids and keyphrase then) adds what
//! else it names: its address (as above), and its other page ids and keyphrase, each for [`LINK_MS`] (30 days) after
//! it was last seen with one of those, at most [`KEYS_MAX`] of each (the first ones stay).  So the player's new
//! browser is turned away while it keeps showing up with the banned keyphrase, and a month after.  A hello that
//! matches only by what was added so is turned away and adds nothing: a keyphrase the player typed into someone
//! else's browser bans that browser for a month at most, and never its owner's own keyphrase or address.  An address
//! never adds anything (whoever shares it would be banned).  None of it stops a player who clears the site's data,
//! leaves its keyphrase (and its cloud saves and its name) behind and changes networks: without accounts no ban can.
//!
//! A new ban takes the place of the bans that name one of the same page ids or keyphrases (for a player without
//! either: the same address), so `ban Bob perm` after `ban Bob 1d` leaves one ban, for good.  Bans, and what they
//! name for a time, go once their time is up, at the next change or listing; so do marks.  The file is written whole:
//! a temporary file beside it (0600), renamed over it.

use std::io::{self, Read, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};

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
/// the longest message a kick or a ban carries (characters)
pub const MSG_MAX: usize = 200;
/// the longest ban short of one for good (ms): ten years
pub const FOR_MAX: i64 = 3650 * DAY;
/// the most page ids, and keyphrases, a ban takes in later, or a mark names: past them a hello adds none (a player
/// who keeps clearing its storage, or a script making up ids, does not grow the file without end; the first stay)
pub const KEYS_MAX: usize = 32;
/// the most marks kept (the oldest go first)
const MARKS_MAX: usize = 5000;
/// the most names of earlier lines a mark keeps, and bans (the latest stay)
const MARK_NAMES_MAX: usize = 8;
/// a key seen again this soon after its time was last set is not written again
const SLACK: i64 = 3_600_000;
const MIN: i64 = 60_000;
const HOUR: i64 = 60 * MIN;
const DAY: i64 = 24 * HOUR;

/// One ban.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Ban {
    /// its number (`unban #4`)
    pub id: u64,
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
    pub fn live(&self, now: i64) -> bool {
        self.until.is_none_or(|u| u > now)
    }
    /// its page's control message: {"t":"ban","msg":...,"until":ms}, without until for good
    pub fn message(&self) -> String {
        let mut m = Map::new();
        m.insert("t".into(), "ban".into());
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
    fn same_player(&self, o: &Keys) -> bool {
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
/// (oldest first), and the bans made of it since its first line, each with whether it is in force
#[derive(Clone, Debug, PartialEq)]
pub struct Marked {
    pub handle: String,
    pub before: Vec<String>,
    pub bans: Vec<(u64, bool)>,
}

impl Mark {
    /// its handle: "c17"
    pub fn handle(&self) -> String {
        format!("c{}", self.n)
    }
}

#[derive(Default, Serialize, Deserialize)]
struct File {
    /// the last ban's number
    next: u64,
    bans: Vec<Ban>,
    /// the last mark's number, and the marks
    #[serde(default)]
    next_mark: u64,
    #[serde(default)]
    marks: Vec<Mark>,
}

/// The bans of a state folder.  Every call takes one lock; the ones that change something write the file under it
/// (rare: the operator's commands, a banned player in a new browser, with a new keyphrase or at a new address, and a
/// `[cheat]` line of a race).
pub struct Bans {
    path: PathBuf,
    key: Vec<u8>,
    file: Mutex<File>,
}

impl Bans {
    /// The bans in `dir` (bans.json, and its key bans.key, made at the first start).  Err: a file that cannot be read.
    pub fn open(dir: &Path) -> io::Result<Bans> {
        let key = load_key(&dir.join("bans.key"))?;
        let path = dir.join("bans.json");
        let file = match std::fs::read(&path) {
            Ok(b) => serde_json::from_slice(&b).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?,
            Err(e) if e.kind() == io::ErrorKind::NotFound => File::default(),
            Err(e) => return Err(e),
        };
        Ok(Bans { path, key, file: Mutex::new(file) })
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

    /// the live ban an address (its hash) is under now, the one that lasts longest
    pub fn by_addr(&self, addr: &str, now: i64) -> Option<Ban> {
        let f = self.file.lock();
        longest(&f.bans, |b| b.live(now) && b.at_addr(addr, now)).map(|i| f.bans[i].clone())
    }

    /// The live ban a hello's page ids or keyphrase are under now, the one that lasts longest.  When it was made with
    /// one of them, what else the hello names is added to it: its other page ids and its keyphrase for `LINK_MS` (up to
    /// `KEYS_MAX` each), the address it came from (its hash) for `ADDR_MS`, or each kept that much longer.  A hello
    /// that matches only by what a ban took in later adds nothing.
    pub fn by_hello(&self, s: &Seen, now: i64) -> Option<Ban> {
        let pages: Vec<String> = s.pages.iter().map(|p| self.page(p)).collect();
        let owners: Vec<String> = s.owner.map(|o| self.owner(o)).into_iter().collect();
        let mut f = self.file.lock();
        let Some(i) = longest(&f.bans, |b| b.live(now) && b.made_for(&pages, &owners)) else {
            return longest(&f.bans, |b| b.live(now) && b.took_in(&pages, &owners, now)).map(|i| f.bans[i].clone());
        };
        let b = &mut f.bans[i];
        let (link, addr) = (b.until_for(LINK_MS, now), b.until_for(ADDR_MS, now));
        let mut changed = false;
        for p in pages.iter().filter(|p| !b.pages.contains(p)) {
            changed |= hold(&mut b.later_pages, p, link, KEYS_MAX);
        }
        for o in owners.iter().filter(|o| !b.owners.contains(o)) {
            changed |= hold(&mut b.later_owners, o, link, KEYS_MAX);
        }
        if let Some(a) = s.addr {
            changed |= hold(&mut b.addrs, a, addr, usize::MAX);
        }
        let ban = b.clone();
        if changed {
            self.save_or_say(&mut f, now);
        }
        Some(ban)
    }

    /// whether `ban` names one of these keys (a connection's: `Bans::keys`) now: the other connections a new ban removes
    pub fn covers(&self, ban: &Ban, k: &Keys, now: i64) -> bool {
        ban.made_for(&k.pages, &k.owners) || ban.took_in(&k.pages, &k.owners, now) || k.addrs.iter().any(|a| ban.at_addr(a, now))
    }

    /// A new ban of the player `k` names (`name` for the operator), with `msg`, until `until` (None: for good).  It
    /// takes the place of the bans that name one of its page ids or keyphrases (a player without either: one of its
    /// addresses); their numbers come back with it.
    pub fn add(&self, name: &str, k: &Keys, msg: &str, until: Option<i64>, now: i64) -> io::Result<(Ban, Vec<u64>)> {
        let mut f = self.file.lock();
        let mut replaced = Vec::new();
        let by_id = !k.pages.is_empty() || !k.owners.is_empty();
        f.bans.retain(|b| {
            let same = if by_id {
                b.made_for(&k.pages, &k.owners) || b.took_in(&k.pages, &k.owners, now)
            } else {
                k.addrs.iter().any(|a| b.addrs.iter().any(|x| &x.h == a))
            };
            if same && b.live(now) {
                replaced.push(b.id);
            }
            !same
        });
        f.next += 1;
        let mut b = Ban {
            id: f.next,
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
        let addr = b.until_for(ADDR_MS, now);
        b.addrs = own.addrs.into_iter().map(|h| Held { h, until: addr }).collect();
        (b.pages, b.owners) = (own.pages, own.owners);
        f.bans.push(b.clone());
        // the marks of the player (a ban by its handle, or of one with a mark by its name or address)
        for m in f.marks.iter_mut().filter(|m| m.at + MARK_MS > now && k.same_player(&m.keys)) {
            m.bans.push(b.id);
            if m.bans.len() > MARK_NAMES_MAX {
                m.bans.remove(0);
            }
        }
        self.save(&mut f, now)?;
        Ok((b, replaced))
    }

    /// The bans `sel` names, lifted: "#4" or "4" (a number), or a name (any case: every ban of that name).
    pub fn remove(&self, sel: &str, now: i64) -> io::Result<Vec<Ban>> {
        let sel = sel.trim();
        let num = sel.strip_prefix('#').unwrap_or(sel).parse::<u64>().ok();
        let mut f = self.file.lock();
        let mut gone = Vec::new();
        f.bans.retain(|b| {
            let hit = match num {
                Some(n) => b.id == n,
                None => b.name.to_lowercase() == sel.to_lowercase(),
            };
            if hit && b.live(now) {
                gone.push(b.clone());
            }
            !hit
        });
        if !gone.is_empty() {
            self.save(&mut f, now)?;
        }
        Ok(gone)
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
        let bans = m.bans.iter().map(|&id| (id, f.bans.iter().any(|b| b.id == id && b.live(now)))).collect();
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

/// the bans whose time is up go, and so do what they name for a time and the marks whose time is up
fn prune(f: &mut File, now: i64) {
    f.bans.retain(|b| b.live(now));
    for b in &mut f.bans {
        for l in [&mut b.addrs, &mut b.later_pages, &mut b.later_owners] {
            l.retain(|x| x.until > now);
        }
    }
    f.marks.retain(|m| m.at + MARK_MS > now);
}

/// how much the file holds: what `list` compares to see whether `prune` took anything
fn counts(f: &File) -> (usize, usize, usize) {
    let held = f.bans.iter().map(|b| b.addrs.len() + b.later_pages.len() + b.later_owners.len()).sum();
    (f.bans.len(), held, f.marks.len())
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
mod tests {
    use super::*;

    const T0: i64 = 1_790_000_000_000;

    fn bans() -> (tempfile::TempDir, Bans) {
        let dir = tempfile::tempdir().unwrap();
        let b = Bans::open(dir.path()).unwrap();
        (dir, b)
    }

    /// a player's keys: its address's hash, its page id, its keyphrase's owner
    fn who(b: &Bans, addr: Option<&str>, page: Option<&str>, owner: Option<&str>) -> Keys {
        b.keys(addr, page, owner)
    }

    /// a hello: its page ids, its keyphrase's owner, its address's hash
    fn hello(b: &Bans, pages: &[&str], owner: Option<&str>, addr: Option<&str>, now: i64) -> Option<Ban> {
        b.by_hello(&Seen { pages, owner, addr }, now)
    }

    #[test]
    fn a_ban_turns_away_its_address_and_its_page() {
        let (_d, b) = bans();
        let (a1, a2) = (b.addr("203.0.113.5"), b.addr("198.51.100.7"));
        assert_ne!(a1, a2);
        assert_eq!(a1.len(), 22);
        let (ban, replaced) =
            b.add("Bob", &who(&b, Some(&a1), Some("page-1"), None), " Ramming,\n3 days ", Some(T0 + 3 * DAY), T0).unwrap();
        assert!(replaced.is_empty());
        assert_eq!((ban.id, ban.msg.as_str()), (1, "Ramming, 3 days"));
        assert_eq!(b.by_addr(&a1, T0 + 1).map(|x| x.id), Some(1));
        assert_eq!(b.by_addr(&a2, T0 + 1), None);
        assert_eq!(hello(&b, &["page-2"], None, Some(&a2), T0 + 1), None);
        // the banned page at another address: that address is banned too
        assert_eq!(hello(&b, &["page-1"], None, Some(&a2), T0 + 1).map(|x| x.id), Some(1));
        assert_eq!(b.by_addr(&a2, T0 + 2).map(|x| x.id), Some(1));
        // and its time is up with the ban's
        assert_eq!(b.by_addr(&a1, T0 + 3 * DAY), None);
        assert_eq!(hello(&b, &["page-1"], None, None, T0 + 3 * DAY), None);
        assert!(b.list(T0 + 3 * DAY).is_empty());
    }

    #[test]
    fn an_address_is_banned_for_a_week_after_it_was_last_the_players_and_never_bans_a_page() {
        let (_d, b) = bans();
        let a = b.addr("203.0.113.5");
        b.add("Bob", &who(&b, Some(&a), Some("p"), None), "", None, T0).unwrap();
        // whoever else comes from that address is turned away, but their page and keyphrase are not banned for it
        assert!(b.by_addr(&a, T0 + DAY).is_some());
        assert_eq!(hello(&b, &["someone-else"], Some("their-keyphrase"), Some(&a), T0 + DAY), None);
        let ban = &b.list(T0 + DAY)[0];
        assert_eq!((ban.pages.len(), ban.later_pages.len(), ban.later_owners.len()), (1, 0, 0));
        assert!(b.by_addr(&a, T0 + ADDR_MS).is_none(), "the address goes after a week");
        // the page comes back from it after two weeks: a week more for the address
        assert!(hello(&b, &["p"], None, Some(&a), T0 + 14 * DAY).is_some());
        assert!(b.by_addr(&a, T0 + 20 * DAY).is_some());
        assert!(b.by_addr(&a, T0 + 21 * DAY).is_none());
        // for good: the page still
        assert!(hello(&b, &["p"], None, None, T0 + 3000 * DAY).is_some());
        assert_eq!(b.list(T0 + 3000 * DAY)[0].addrs, vec![]);
    }

    #[test]
    fn a_banned_keyphrase_brings_in_a_new_browser_for_a_month_and_that_spreads_no_further() {
        let (_d, b) = bans();
        let (home, phone, cafe) = (b.addr("203.0.113.5"), b.addr("198.51.100.7"), b.addr("192.0.2.1"));
        b.add("Bob", &who(&b, Some(&home), Some("pc"), Some("bob-key")), "", None, T0).unwrap();
        // the keyphrase in a new browser on another network: turned away, and that browser and network are taken in
        assert_eq!(hello(&b, &["phone"], Some("bob-key"), Some(&phone), T0 + 1).map(|x| x.id), Some(1));
        let ban = &b.list(T0 + 2)[0];
        assert_eq!((ban.pages.len(), ban.later_pages.len(), ban.owners.len(), ban.addrs.len()), (1, 1, 1, 2));
        assert!(b.by_addr(&phone, T0 + 2).is_some());
        // that browser alone, without the keyphrase: turned away, but it brings in nothing (a friend's browser, say:
        // not its network, not its owner's own keyphrase)
        assert!(hello(&b, &["phone"], Some("friend-key"), Some(&cafe), T0 + 3).is_some());
        assert!(b.by_addr(&cafe, T0 + 4).is_none());
        assert!(hello(&b, &["friend-pc"], Some("friend-key"), None, T0 + 4).is_none());
        // for a month after it was last seen with the keyphrase (seen alone, it is not kept longer)
        assert!(hello(&b, &["phone"], None, None, T0 + LINK_MS).is_some());
        assert!(hello(&b, &["phone"], None, None, T0 + 1 + LINK_MS).is_none());
        // seen with it again: another month
        assert!(hello(&b, &["phone"], Some("bob-key"), None, T0 + 40 * DAY).is_some());
        assert!(hello(&b, &["phone"], None, None, T0 + 69 * DAY).is_some());
        assert!(hello(&b, &["phone"], None, None, T0 + 70 * DAY + 1).is_none());
        assert_eq!(b.list(T0 + 71 * DAY)[0].later_pages, vec![], "gone from the file");
        // a banned page with a new keyphrase: that keyphrase for a month, but not a browser it is used in then
        assert!(hello(&b, &["pc"], Some("bob-new-key"), None, T0 + 80 * DAY).is_some());
        assert!(hello(&b, &["fresh"], Some("bob-new-key"), None, T0 + 81 * DAY).is_some());
        assert!(hello(&b, &["fresh"], None, None, T0 + 82 * DAY).is_none());
        // a page that found the banned id beside its new one: the new one for a month
        assert!(hello(&b, &["cleared", "pc"], None, None, T0 + 90 * DAY).is_some());
        assert!(hello(&b, &["cleared"], None, None, T0 + 91 * DAY).is_some());
        let ban = &b.list(T0 + 92 * DAY)[0];
        assert_eq!((ban.later_pages.len(), ban.later_owners.len()), (1, 1));
        // what the ban was made with lasts as long as it does
        assert!(hello(&b, &["pc"], None, None, T0 + 3000 * DAY).is_some());
        assert!(hello(&b, &["x"], Some("bob-key"), None, T0 + 3000 * DAY).is_some());
        // a ban for a week: what it takes in goes with it
        let (_d2, b) = bans();
        b.add("Eve", &who(&b, None, Some("e"), Some("eve-key")), "", Some(T0 + 7 * DAY), T0).unwrap();
        assert!(hello(&b, &["e2"], Some("eve-key"), None, T0 + 1).is_some());
        assert_eq!(b.list(T0 + 2)[0].later_pages[0].until, T0 + 7 * DAY);
        // the file names nothing as it came
        let text = std::fs::read_to_string(&b.path).unwrap();
        assert!(!text.contains("eve-key") && !text.contains("\"e2\""), "{text}");
    }

    #[test]
    fn a_ban_takes_in_at_most_keys_max_pages_and_keyphrases_and_keeps_the_first() {
        let (_d, b) = bans();
        b.add("Bob", &who(&b, None, Some("p0"), Some("k0")), "", None, T0).unwrap();
        for i in 1..100 {
            let (p, k) = (format!("p{i}"), format!("k{i}"));
            assert!(hello(&b, &[&p], Some("k0"), None, T0 + i).is_some(), "{p} with the banned keyphrase");
            assert!(hello(&b, &["p0"], Some(&k), None, T0 + i).is_some(), "{k} in the banned page");
        }
        let ban = &b.list(T0 + 100)[0];
        assert_eq!((ban.later_pages.len(), ban.later_owners.len()), (KEYS_MAX, KEYS_MAX));
        assert!(hello(&b, &["p1"], None, None, T0 + 101).is_some() && hello(&b, &["p32"], None, None, T0 + 101).is_some());
        assert!(hello(&b, &["p33"], None, None, T0 + 101).is_none(), "past the cap: turned away that time, not kept");
        assert!(hello(&b, &["x"], Some("k32"), None, T0 + 101).is_some());
        assert!(hello(&b, &["x"], Some("k33"), None, T0 + 101).is_none());
    }

    #[test]
    fn a_new_ban_of_the_same_page_or_keyphrase_takes_the_place_of_the_old_one() {
        let (_d, b) = bans();
        let a = b.addr("203.0.113.5");
        b.add("Bob", &who(&b, Some(&a), Some("p"), None), "a day", Some(T0 + DAY), T0).unwrap();
        b.add("Eve", &who(&b, Some(&a), Some("q"), None), "eve", Some(T0 + DAY), T0).unwrap();
        let (ban, replaced) = b.add("Bob", &who(&b, Some(&a), Some("p"), None), "for good", None, T0 + 1).unwrap();
        assert_eq!((ban.id, replaced), (3, vec![1]));
        let names: Vec<_> = b.list(T0 + 2).iter().map(|x| (x.id, x.name.clone())).collect();
        assert_eq!(names, vec![(2, "Eve".to_string()), (3, "Bob".to_string())]);
        // a player without a page id: by address
        let c = b.addr("192.0.2.1");
        b.add("Old", &who(&b, Some(&c), None, None), "", Some(T0 + DAY), T0).unwrap();
        let (_, replaced) = b.add("Old", &who(&b, Some(&c), None, None), "", None, T0).unwrap();
        assert_eq!(replaced, vec![4]);
        // the longest of two that match is the one a page is told
        assert_eq!(b.by_addr(&a, T0 + 2).map(|x| x.until), Some(None));
        // the same keyphrase in another browser
        b.add("Kim", &who(&b, None, Some("k1"), Some("kim-key")), "", Some(T0 + DAY), T0).unwrap();
        assert_eq!(b.add("Kim", &who(&b, None, Some("k2"), Some("kim-key")), "", None, T0 + 1).unwrap().1, vec![6]);
        // a player in two browsers at once (its keys together): both, each once
        let mut two = who(&b, Some(&c), Some("t1"), Some("two-key"));
        two.extend(who(&b, Some(&c), Some("t2"), Some("two-key")));
        let (ban, _) = b.add("Two", &two, "", None, T0).unwrap();
        assert_eq!((ban.pages.len(), ban.owners.len(), ban.addrs.len()), (2, 1, 1));
    }

    #[test]
    fn bans_are_lifted_by_number_or_name() {
        let (_d, b) = bans();
        let a = b.addr("203.0.113.5");
        b.add("Bob", &who(&b, Some(&a), Some("p"), None), "", None, T0).unwrap();
        b.add("bob", &who(&b, None, Some("q"), None), "", None, T0).unwrap();
        b.add("Eve", &who(&b, None, Some("r"), None), "", None, T0).unwrap();
        assert_eq!(b.remove("#3", T0).unwrap().len(), 1);
        assert_eq!(b.remove("BOB", T0).unwrap().iter().map(|x| x.id).collect::<Vec<_>>(), vec![1, 2]);
        assert!(b.remove("4", T0).unwrap().is_empty());
        assert!(b.by_addr(&a, T0).is_none());
    }

    #[test]
    fn a_cheat_lines_player_is_marked_for_a_week_by_one_handle() {
        let (_d, b) = bans();
        let a = b.addr("203.0.113.5");
        assert_eq!(b.mark("Bob", who(&b, Some(&a), Some("bob-page"), Some("bob-key")), T0).unwrap().handle, "c1");
        assert_eq!(b.mark("Eve", who(&b, None, Some("eve-page"), None), T0 + 1).unwrap().handle, "c2");
        // Bob again, in another browser with his keyphrase, and at another address: the same handle, from then
        let a2 = b.addr("198.51.100.7");
        assert_eq!(b.mark("Bobby", who(&b, Some(&a2), Some("bob-phone"), Some("bob-key")), T0 + DAY).unwrap().handle, "c1");
        let m = b.marked("C1", T0 + DAY).expect("Bob's mark");
        assert_eq!((m.name.as_str(), m.keys.addrs.len(), m.keys.pages.len(), m.keys.owners.len()), ("Bobby", 2, 2, 1));
        assert!(b.marked("c3", T0).is_none() && b.marked("x1", T0).is_none() && b.marked("c", T0).is_none());
        // a week after its last line
        assert!(b.marked("c1", T0 + DAY + MARK_MS - 1).is_some());
        assert!(b.marked("c1", T0 + DAY + MARK_MS).is_none());
        assert!(b.marked("c2", T0 + 1 + MARK_MS).is_none());
        // a ban by its keys: whoever has one of them
        let (ban, _) = b.add(&m.name, &m.keys, "", None, T0 + DAY).unwrap();
        assert!(hello(&b, &["bob-phone"], None, None, T0 + DAY).is_some());
        assert!(b.covers(&ban, &who(&b, None, Some("new"), Some("bob-key")), T0 + DAY));
        assert!(b.by_addr(&a2, T0 + DAY).is_some());
        // the file: hashes only, and the marks outlive a restart; a new handle after the old ones
        let text = std::fs::read_to_string(&b.path).unwrap();
        assert!(!text.contains("bob-") && !text.contains("203.0.113"), "{text}");
        // (Bob's next lines at once, nothing new of him: the file is not written again)
        std::fs::write(&b.path, "kept").unwrap();
        for i in 1..=50 {
            assert_eq!(b.mark("Bobby", who(&b, Some(&a2), Some("bob-phone"), Some("bob-key")), T0 + DAY + i).unwrap().handle, "c1");
        }
        assert_eq!(std::fs::read_to_string(&b.path).unwrap(), "kept");
        std::fs::write(&b.path, text).unwrap();
        let b = Bans::open(b.path.parent().unwrap()).unwrap();
        assert_eq!(b.marked("c1", T0 + DAY).map(|m| m.name), Some("Bobby".into()));
        assert_eq!(b.mark("Kim", who(&b, None, Some("kim"), None), T0 + DAY).unwrap().handle, "c3");
        // their time up: gone from the file
        b.list(T0 + DAY + MARK_MS);
        let file: Value = serde_json::from_str(&std::fs::read_to_string(&b.path).unwrap()).unwrap();
        assert_eq!(file["marks"].as_array().map(Vec::len), Some(0), "{file}");
    }

    // a marked player back under other names (one ban of it ended, one in force): its earlier lines' names, each
    // once, and its bans, by the handle or by name
    #[test]
    fn a_mark_says_the_players_earlier_names_and_its_bans() {
        let (_d, b) = bans();
        let k = || who(&b, None, Some("jek-page"), None);
        assert_eq!(b.mark("ChinaJek", k(), T0).unwrap(), Marked { handle: "c1".into(), before: vec![], bans: vec![] });
        let keys = b.marked("c1", T0).unwrap().keys;
        let (one, _) = b.add("ChinaJek", &keys, "", Some(T0 + DAY), T0).unwrap();
        assert_eq!(b.mark("ChinaJek", k(), T0 + 1).unwrap().bans, [(one.id, true)]);
        let m = b.mark("SuperCharger", k(), T0 + 2 * DAY).unwrap();
        assert_eq!(m, Marked { handle: "c1".into(), before: vec!["ChinaJek".into()], bans: vec![(one.id, false)] });
        assert_eq!(b.mark("Zooom", k(), T0 + 2 * DAY + 1).unwrap().before, ["ChinaJek", "SuperCharger"]);
        assert_eq!(b.mark("chinajek", k(), T0 + 2 * DAY + 2).unwrap().before, ["SuperCharger", "Zooom"]);
        // banned by its name (not the handle): the mark has that ban too
        let (two, _) = b.add("chinajek", &k(), "", None, T0 + 3 * DAY).unwrap();
        let m = b.mark("chinajek", k(), T0 + 3 * DAY).unwrap();
        assert_eq!(m.bans, [(one.id, false), (two.id, true)]);
        // kept over a restart
        let b = Bans::open(b.path.parent().unwrap()).unwrap();
        let m = b.marked("c1", T0 + 3 * DAY).unwrap();
        assert_eq!((m.before.len(), m.bans), (2, vec![one.id, two.id]));
    }

    #[test]
    fn the_file_keeps_hashes_only_and_outlives_a_restart() {
        let dir = tempfile::tempdir().unwrap();
        let b = Bans::open(dir.path()).unwrap();
        let a = b.addr("203.0.113.5");
        b.add("Bob", &who(&b, Some(&a), Some("page-secret"), Some("owner-secret")), "msg", Some(T0 + DAY), T0).unwrap();
        let text = std::fs::read_to_string(dir.path().join("bans.json")).unwrap();
        assert!(!text.contains("203.0.113") && !text.contains("page-secret") && !text.contains("owner-secret"), "{text}");
        use std::os::unix::fs::PermissionsExt;
        for f in ["bans.json", "bans.key"] {
            assert_eq!(std::fs::metadata(dir.path().join(f)).unwrap().permissions().mode() & 0o777, 0o600, "{f}");
        }
        drop(b);
        let b = Bans::open(dir.path()).unwrap();
        assert_eq!(b.addr("203.0.113.5"), a, "the same key");
        assert_eq!(hello(&b, &["page-secret"], None, None, T0 + 1).map(|x| x.msg), Some("msg".into()));
        assert!(hello(&b, &["elsewhere"], Some("owner-secret"), None, T0 + 1).is_some());
        let (next, _) = b.add("Eve", &who(&b, None, Some("x"), None), "", None, T0).unwrap();
        assert_eq!(next.id, 2);
        // a file from before keyphrases and marks
        let old = r#"{"next":1,"bans":[{"id":1,"name":"Old","at":0,"until":null,"pages":["x"],"addrs":[{"h":"y","until":9}]}]}"#;
        std::fs::write(dir.path().join("bans.json"), old).unwrap();
        let ban = Bans::open(dir.path()).unwrap().list(0)[0].clone();
        assert_eq!((ban.owners.len(), ban.later_pages.len(), ban.addrs[0].until), (0, 0, 9));
        // a broken file: not taken for no bans
        std::fs::write(dir.path().join("bans.json"), "{").unwrap();
        assert!(Bans::open(dir.path()).is_err());
    }

    #[test]
    fn messages_for_the_page() {
        let (_d, b) = bans();
        let (ban, _) = b.add("B", &who(&b, None, Some("p"), None), "Bye \"now\"", Some(T0 + DAY), T0).unwrap();
        assert_eq!(ban.message(), format!(r#"{{"t":"ban","msg":"Bye \"now\"","until":{}}}"#, T0 + DAY));
        let (ban, _) = b.add("B", &who(&b, None, Some("p"), None), "", None, T0).unwrap();
        assert_eq!(ban.message(), r#"{"t":"ban","msg":""}"#);
        assert_eq!(kick_message("a\u{202E}b\tc"), r#"{"t":"kick","msg":"ab c"}"#);
        assert_eq!(clean_msg(&"x".repeat(300)).len(), MSG_MAX);
    }

    #[test]
    fn how_long_as_the_operator_writes_it() {
        assert_eq!(parse_for("30m"), Ok(Some(30 * MIN)));
        assert_eq!(parse_for("30min"), Ok(Some(30 * MIN)));
        assert_eq!(parse_for("12h"), Ok(Some(12 * HOUR)));
        assert_eq!(parse_for("3D"), Ok(Some(3 * DAY)));
        assert_eq!(parse_for("2w"), Ok(Some(14 * DAY)));
        assert_eq!(parse_for("perm"), Ok(None));
        assert_eq!(parse_for("forever"), Ok(None));
        for bad in ["", "3", "d", "0d", "-1d", "1.5d", "3 days", "99999999999999w", "3651d"] {
            assert!(parse_for(bad).is_err(), "{bad}");
        }
        assert_eq!(span(45 * MIN), "45 min");
        assert_eq!(span(5 * HOUR + 10 * MIN), "5 h 10 min");
        assert_eq!(span(3 * DAY - 1), "3 d");
        assert_eq!(span(2 * DAY + 23 * HOUR), "2 d 23 h");
        assert_eq!(span(12 * HOUR), "12 h");
        assert_eq!(when(1_793_109_900_000), "2026-10-27 14:05 UTC");
    }
}
