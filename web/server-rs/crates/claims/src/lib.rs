//! Claimed names (docs/web/PLAYING.md, "Your name"): a player claims the name it plays under with its keyphrase, the
//! one that keeps its cloud saves (nfsu2-vault), and while the claim holds nobody else plays under that name: the
//! Online lobby gives anyone else the name with four digits in place of its end (nfsu2-online lobby.rs), free roam's
//! hub links nobody else under it (nfsu2-roam hub.rs), and EA Messenger keeps its lists only for a claimed name, with
//! the keyphrase that claimed it (nfsu2-online buddy.rs).
//!
//! - A name is compared in its Online form ([`clean`]: the letters, digits, - and _ of what was typed, any other run
//!   of characters one _, at most 15, as runtime/online.c makes the game's name), in any case.
//! - The game's names for nobody in particular ([`UNCLAIMABLE`]: NAME, the default profile name that hundreds of
//!   players keep, and Player) and a name that reads as a slur (nfsu2-names) are never claimed.
//! - A claim holds [`HOLD_DAYS`] after its owner last played under it ([`Claims::seen`]).
//! - One name a keyphrase.  Claiming another is a change: the first is free, then one every [`CHANGE_DAYS`]; the
//!   name before stays the owner's for [`GRACE_DAYS`] more (its friends still have it on their lists for a while).
//! - The owner is a keyphrase, by a hash of its token ([`owner_of_token`]): the page sends the token where it claims
//!   (/names) and in its hello to the switch (nfsu2-lan: a frame's `FrameCtx::owner`), never the keyphrase.  A
//!   keyphrase deleted from the cloud lets its names go ([`Claims::release`]).
//!
//! Kept in <state>/names.json (the names, their owners' hashes, when they were made and last played), written whole
//! to a temporary file of 0600 and renamed over the old one ([`Claims::save`]).

use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use parking_lot::RwLock;
use serde_json::Value;
use sha2::{Digest, Sha256};

#[cfg(test)]
mod tests;

pub const DAY_MS: i64 = 86_400_000;
/// a claim holds this long after its owner last played under it
pub const HOLD_DAYS: i64 = 180;
/// at most one change of name in this many days (the first is free)
pub const CHANGE_DAYS: i64 = 7;
/// the name before a change stays its owner's this long
pub const GRACE_DAYS: i64 = 14;
/// the names nobody claims (lower case): the game's default profile name, and its name for nobody in particular
pub const UNCLAIMABLE: &[&str] = &["name", "player"];
/// claims kept at most (the dead ones go first)
pub const MAX_CLAIMS: usize = 500_000;
/// a claim's `seen` moves on at most this often (each move is a write of the file)
const SEEN_STEP_MS: i64 = 3_600_000;

/// a keyphrase, as the claims know it: 22 characters of base64url
pub type Owner = Arc<str>;

/// The Online form of a name, as runtime/online.c `clean` makes the game's: the letters, digits, - and _, any other
/// run of characters one _ (none at either end), cut to 15; one or two characters padded with _.  None: nothing of a
/// name in it.
pub fn clean(name: &str) -> Option<String> {
    let mut out = String::with_capacity(15);
    let mut gap = false;
    for ch in name.chars() {
        if out.len() >= 15 {
            break;
        }
        if !(ch.is_ascii_alphanumeric() || ch == '_' || ch == '-') {
            gap = true;
            continue;
        }
        if gap && !out.is_empty() && out.len() < 14 {
            out.push('_');
        }
        gap = false;
        out.push(ch);
    }
    if out.is_empty() {
        return None;
    }
    while out.len() < 3 {
        out.push('_');
    }
    Some(out)
}

/// what a name is compared by: its Online form in lower case
pub fn canonical(name: &str) -> Option<String> {
    clean(name).map(|s| s.to_ascii_lowercase())
}

/// The name as typed or its Online form reads as a slur (nfsu2-names): both, since the form leaves out what is no
/// letter or digit of the game's ("Nіgger" with a Cyrillic і reads as one typed, its form "N_gger" would not) and
/// joins what spaces kept apart.
pub fn reads_as_slur(typed: &str, shown: &str) -> bool {
    nfsu2_names::blocked_name(typed) || nfsu2_names::blocked_name(shown)
}

/// the owner a keyphrase's token stands for (`Bearer` at /names, "claim" in the switch's hello): None when it is no
/// token (43 characters of base64url, as /vault takes them)
pub fn owner_of_token(token: &str) -> Option<Owner> {
    let ok = token.len() == 43 && token.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_');
    ok.then(|| owner_of_id(&Sha256::digest(token.as_bytes()).into()))
}

/// the owner of the keyphrase kept under `id` (nfsu2-vault's id: the token's SHA-256)
pub fn owner_of_id(id: &[u8; 32]) -> Owner {
    let mut h = Sha256::new();
    h.update(b"nfsu2 claim\0");
    h.update(id);
    Arc::from(&nfsu2_util::js::base64url(&h.finalize())[..22])
}

/// What a name is to whoever asks (its owner, or None for nobody's)
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Status {
    /// nothing of a name in it
    Invalid,
    /// NAME or Player: nobody claims it
    Default,
    /// it reads as a slur (nfsu2-names)
    Blocked,
    Free,
    /// the asker's until then: its current name, or the one before its last change (for `GRACE_DAYS`)
    Yours {
        until: i64,
        current: bool,
    },
    /// another keyphrase's
    Taken,
}

/// an owner's current name: as shown, until when it holds, and from when it may change (0: now)
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Mine {
    pub name: String,
    pub until: i64,
    pub next_change: i64,
}

/// why a claim was not made
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Refusal {
    Invalid,
    Default,
    Blocked,
    Taken,
    /// the owner changed its name less than `CHANGE_DAYS` ago: its next change then
    TooSoon {
        next: i64,
    },
    Full,
}

#[derive(Clone, Debug)]
struct Claim {
    /// as shown (its Online form, the case its owner typed)
    name: String,
    owner: Owner,
    made: i64,
    /// its owner last played under it (or claimed it)
    seen: i64,
    /// 0: held for `HOLD_DAYS` after `seen`; else the end of the grace of a name its owner changed from
    until: i64,
}

impl Claim {
    fn end(&self) -> i64 {
        if self.until > 0 { self.until } else { self.seen + HOLD_DAYS * DAY_MS }
    }
    fn live(&self, now: i64) -> bool {
        now < self.end()
    }
}

#[derive(Clone, Debug, Default)]
struct OwnerRec {
    /// its current name (lower case)
    current: Option<String>,
    /// its last change (0: none yet)
    changed: i64,
    /// the name before its last change (lower case): its Messenger lists move from there (`previous`)
    prev: Option<String>,
}

#[derive(Default)]
struct Inner {
    /// lower case -> claim (dead ones too, until `prune`)
    names: HashMap<String, Claim>,
    owners: HashMap<Owner, OwnerRec>,
}

impl Inner {
    fn live(&self, key: &str, now: i64) -> Option<&Claim> {
        self.names.get(key).filter(|c| c.live(now))
    }

    /// the owner's live current name (lower case)
    fn current(&self, owner: &str, now: i64) -> Option<&String> {
        let k = self.owners.get(owner)?.current.as_ref()?;
        self.live(k, now).filter(|c| c.until == 0 && &*c.owner == owner).map(|_| k)
    }

    fn mine(&self, owner: &str, now: i64) -> Option<Mine> {
        let k = self.current(owner, now)?;
        let c = &self.names[k];
        let changed = self.owners.get(owner).map_or(0, |r| r.changed);
        Some(Mine { name: c.name.clone(), until: c.end(), next_change: if changed > 0 { changed + CHANGE_DAYS * DAY_MS } else { 0 } })
    }

    /// the dead claims go, and owners with nothing left: true when any did
    fn prune(&mut self, now: i64) -> bool {
        let n = self.names.len();
        self.names.retain(|_, c| c.live(now));
        let names = &self.names;
        self.owners.retain(|o, r| {
            if r.current.as_ref().is_some_and(|k| !names.get(k).is_some_and(|c| c.owner == *o)) {
                r.current = None;
            }
            // (a change within CHANGE_DAYS still counts against the next one)
            r.current.is_some() || now < r.changed + CHANGE_DAYS * DAY_MS
        });
        self.names.len() != n
    }
}

/// The claimed names: shared by the server's routes (/names), the Online host and free roam's hub, each reading
/// under a lock of its own moment.
pub struct Claims {
    path: Option<PathBuf>,
    inner: RwLock<Inner>,
    /// one more each time a name's owner changes (a claim, a change, a release, a claim's end): readers that keep what
    /// they found (the hub) look again
    generation: AtomicU64,
    dirty: AtomicBool,
}

impl Claims {
    /// the claims of <dir>/names.json (none yet: an empty file is made at the first claim)
    pub fn open(dir: &Path, now: i64) -> Claims {
        let c = Claims::new(Some(dir.join("names.json")));
        if let Ok(text) = std::fs::read_to_string(dir.join("names.json")) {
            c.read(&text, now);
        }
        c
    }

    /// claims kept nowhere (tests, a server without a state folder)
    pub fn in_memory() -> Claims {
        Claims::new(None)
    }

    fn new(path: Option<PathBuf>) -> Claims {
        Claims { path, inner: RwLock::new(Inner::default()), generation: AtomicU64::new(1), dirty: AtomicBool::new(false) }
    }

    fn changed(&self) {
        self.generation.fetch_add(1, Ordering::Relaxed);
        self.dirty.store(true, Ordering::Relaxed);
    }

    /// see the field
    pub fn generation(&self) -> u64 {
        self.generation.load(Ordering::Relaxed)
    }

    /// how many names are claimed now
    pub fn count(&self, now: i64) -> usize {
        self.inner.read().names.values().filter(|c| c.live(now)).count()
    }

    /// what `name` is to `owner` (None: to nobody's keyphrase)
    pub fn status(&self, name: &str, owner: Option<&str>, now: i64) -> Status {
        let Some(shown) = clean(name) else { return Status::Invalid };
        let key = shown.to_ascii_lowercase();
        if UNCLAIMABLE.contains(&key.as_str()) {
            return Status::Default;
        }
        if reads_as_slur(name, &shown) {
            return Status::Blocked;
        }
        let g = self.inner.read();
        match g.live(&key, now) {
            None => Status::Free,
            Some(c) if owner.is_some_and(|o| *c.owner == *o) => Status::Yours { until: c.end(), current: c.until == 0 },
            Some(_) => Status::Taken,
        }
    }

    /// the keyphrase `name` is claimed by now (None: nobody's)
    pub fn holder(&self, name: &str, now: i64) -> Option<Owner> {
        let key = canonical(name)?;
        self.inner.read().live(&key, now).map(|c| c.owner.clone())
    }

    /// whoever has `owner` may play under `name`: nobody claimed it, or that keyphrase did
    pub fn may_use(&self, name: &str, owner: Option<&str>, now: i64) -> bool {
        match self.holder(name, now) {
            None => true,
            Some(h) => owner.is_some_and(|o| *h == *o),
        }
    }

    /// `owner`'s current name
    pub fn mine(&self, owner: &str, now: i64) -> Option<Mine> {
        self.inner.read().mine(owner, now)
    }

    /// the name `owner` had before its last change, while that change is younger than `GRACE_DAYS` (lower case)
    pub fn previous(&self, owner: &str, now: i64) -> Option<String> {
        let g = self.inner.read();
        let r = g.owners.get(owner)?;
        (now < r.changed + GRACE_DAYS * DAY_MS).then(|| r.prev.clone()).flatten()
    }

    /// `owner` claims `name`: its name from now on (a change when it had another)
    pub fn claim(&self, name: &str, owner: &Owner, now: i64) -> Result<Mine, Refusal> {
        let shown = clean(name).ok_or(Refusal::Invalid)?;
        let key = shown.to_ascii_lowercase();
        if UNCLAIMABLE.contains(&key.as_str()) {
            return Err(Refusal::Default);
        }
        if reads_as_slur(name, &shown) {
            return Err(Refusal::Blocked);
        }
        let mut g = self.inner.write();
        match g.live(&key, now) {
            Some(c) if c.owner != *owner => return Err(Refusal::Taken),
            Some(c) if c.until == 0 => {
                // its own current name again: as it is typed now, and played
                let c = g.names.get_mut(&key).expect("live");
                if c.name != shown || now - c.seen >= SEEN_STEP_MS {
                    c.name = shown;
                    c.seen = now;
                    self.dirty.store(true, Ordering::Relaxed);
                }
                return Ok(g.mine(owner, now).expect("current"));
            }
            // free, or the name it changed from (back to it: a change too)
            _ => {}
        }
        if let Some(old) = g.current(owner, now).cloned() {
            let rec = g.owners.get(owner).cloned().unwrap_or_default();
            if rec.changed > 0 && now < rec.changed + CHANGE_DAYS * DAY_MS {
                return Err(Refusal::TooSoon { next: rec.changed + CHANGE_DAYS * DAY_MS });
            }
            let c = g.names.get_mut(&old).expect("current");
            c.until = now + GRACE_DAYS * DAY_MS;
            let r = g.owners.entry(owner.clone()).or_default();
            r.prev = Some(old);
            r.changed = now;
        }
        if g.names.len() >= MAX_CLAIMS && !g.names.contains_key(&key) {
            g.prune(now);
            if g.names.len() >= MAX_CLAIMS {
                return Err(Refusal::Full);
            }
        }
        g.names.insert(key.clone(), Claim { name: shown, owner: owner.clone(), made: now, seen: now, until: 0 });
        g.owners.entry(owner.clone()).or_default().current = Some(key);
        let mine = g.mine(owner, now).expect("just claimed");
        drop(g);
        self.changed();
        Ok(mine)
    }

    /// `owner` played under `name` (the Online lobby, free roam): a claim of its current name holds `HOLD_DAYS` from
    /// now
    pub fn seen(&self, name: &str, owner: &str, now: i64) {
        let Some(key) = canonical(name) else { return };
        let due = self.inner.read().live(&key, now).is_some_and(|c| *c.owner == *owner && c.until == 0 && now - c.seen >= SEEN_STEP_MS);
        if !due {
            return;
        }
        if let Some(c) = self.inner.write().names.get_mut(&key).filter(|c| *c.owner == *owner && c.until == 0) {
            c.seen = now;
            self.dirty.store(true, Ordering::Relaxed);
        }
    }

    /// a keyphrase gone from the cloud: its names are free.  How many there were.
    pub fn release(&self, owner: &str) -> usize {
        let mut g = self.inner.write();
        let n = g.names.len();
        g.names.retain(|_, c| *c.owner != *owner);
        g.owners.remove(owner);
        let gone = n - g.names.len();
        drop(g);
        if gone > 0 {
            self.changed();
        }
        gone
    }

    /// the operator frees a name (the control socket): whose it was
    pub fn unclaim(&self, name: &str) -> Option<String> {
        let key = canonical(name)?;
        let mut g = self.inner.write();
        let c = g.names.remove(&key)?;
        if let Some(r) = g.owners.get_mut(&c.owner)
            && r.current.as_deref() == Some(key.as_str())
        {
            r.current = None;
        }
        drop(g);
        self.changed();
        Some(c.name)
    }

    /// the dead claims go (the server's minute timer): true when any did, and their readers look again
    pub fn prune(&self, now: i64) -> bool {
        let gone = self.inner.write().prune(now);
        if gone {
            self.changed();
        }
        gone
    }

    /// names.json's text
    pub fn serialize(&self, now: i64) -> String {
        let g = self.inner.read();
        let mut names: Vec<&Claim> = g.names.values().filter(|c| c.live(now)).collect();
        names.sort_by(|a, b| a.made.cmp(&b.made).then_with(|| a.name.cmp(&b.name)));
        let mut s =
            String::from("{\"v\":1,\"note\":\"claimed names: the keyphrase hash that claimed each, when, last played\",\"claims\":[");
        for (i, c) in names.iter().enumerate() {
            if i > 0 {
                s.push(',');
            }
            s.push_str("{\"name\":");
            nfsu2_util::json::write_str(&mut s, &c.name);
            s.push_str(",\"owner\":");
            nfsu2_util::json::write_str(&mut s, &c.owner);
            s.push_str(&format!(",\"made\":{},\"seen\":{}", c.made, c.seen));
            if c.until > 0 {
                s.push_str(&format!(",\"until\":{}", c.until));
            }
            s.push('}');
        }
        s.push_str("],\"owners\":[");
        let mut owners: Vec<(&Owner, &OwnerRec)> = g.owners.iter().filter(|(_, r)| r.changed > 0).collect();
        owners.sort_by(|a, b| a.0.cmp(b.0));
        for (i, (o, r)) in owners.iter().enumerate() {
            if i > 0 {
                s.push(',');
            }
            s.push_str("{\"owner\":");
            nfsu2_util::json::write_str(&mut s, o);
            s.push_str(&format!(",\"changed\":{}", r.changed));
            if let Some(p) = &r.prev {
                s.push_str(",\"prev\":");
                nfsu2_util::json::write_str(&mut s, p);
            }
            s.push('}');
        }
        s.push_str("]}");
        s
    }

    /// names.json read (what is not a live claim of a claimable name is left out)
    fn read(&self, text: &str, now: i64) {
        let Ok(j) = serde_json::from_str::<Value>(text) else { return };
        let mut g = self.inner.write();
        let num = |o: &serde_json::Map<String, Value>, k: &str| {
            o.get(k).and_then(Value::as_f64).filter(|t| (0.0..1e15).contains(t)).map(|t| t as i64)
        };
        let owner = |o: &serde_json::Map<String, Value>| {
            o.get("owner")
                .and_then(Value::as_str)
                .filter(|s| s.len() == 22 && s.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_'))
                .map(Owner::from)
        };
        for e in j.get("claims").and_then(Value::as_array).into_iter().flatten() {
            let Some(o) = e.as_object() else { continue };
            let (Some(name), Some(who), Some(made), Some(seen)) =
                (o.get("name").and_then(Value::as_str).and_then(clean), owner(o), num(o, "made"), num(o, "seen"))
            else {
                continue;
            };
            let key = name.to_ascii_lowercase();
            if UNCLAIMABLE.contains(&key.as_str()) || nfsu2_names::blocked_name(&name) {
                continue;
            }
            let c = Claim { name, owner: who, made, seen, until: num(o, "until").unwrap_or(0) };
            if c.live(now) {
                g.names.insert(key, c);
            }
        }
        // each owner's current name: its newest held one (two in a file edited by hand: the older goes)
        let mut cur: HashMap<Owner, (i64, String)> = HashMap::new();
        for (k, c) in &g.names {
            if c.until == 0 {
                let e = cur.entry(c.owner.clone()).or_insert((c.made, k.clone()));
                if c.made > e.0 {
                    *e = (c.made, k.clone());
                }
            }
        }
        g.names.retain(|k, c| c.until > 0 || cur.get(&c.owner).is_some_and(|(_, ck)| ck == k));
        for (o, (_, k)) in cur {
            g.owners.entry(o).or_default().current = Some(k);
        }
        for e in j.get("owners").and_then(Value::as_array).into_iter().flatten() {
            let Some(o) = e.as_object() else { continue };
            let (Some(who), Some(changed)) = (owner(o), num(o, "changed")) else { continue };
            let r = g.owners.entry(who).or_default();
            r.changed = changed;
            r.prev = o.get("prev").and_then(Value::as_str).and_then(canonical);
        }
        g.prune(now);
    }

    /// names.json written, when anything changed since the last time (the folder and the file made private)
    pub fn save(&self, now: i64) -> Result<bool, String> {
        let Some(path) = &self.path else { return Ok(false) };
        if !self.dirty.swap(false, Ordering::Relaxed) {
            return Ok(false);
        }
        let body = self.serialize(now);
        write_atomic(path, body.as_bytes()).inspect_err(|_| self.dirty.store(true, Ordering::Relaxed))?;
        Ok(true)
    }
}

/// the folder (0700 when it is made), a temporary file (0600), then the rename over `file`
fn write_atomic(file: &Path, body: &[u8]) -> Result<(), String> {
    let dir = file.parent().ok_or("no folder")?;
    let mut b = std::fs::DirBuilder::new();
    b.recursive(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        b.mode(0o700);
    }
    b.create(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let tmp = format!("{}.tmp-{}", file.display(), std::process::id());
    let mut o = std::fs::OpenOptions::new();
    o.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        o.mode(0o600);
    }
    let mut f = o.open(&tmp).map_err(|e| format!("{tmp}: {e}"))?;
    f.write_all(body).map_err(|e| format!("{tmp}: {e}"))?;
    drop(f);
    std::fs::rename(&tmp, file).map_err(|e| format!("{}: {e}", file.display()))
}
