//! Keyphrase saves (`/vault`): a player's saves, kept on the server under a keyphrase only the player has.
//!
//! The page does everything that knows what a save is (web/src/app/vault.ts): it makes the keyphrase, derives a key
//! and a token from it, packs the save folder (but the screenshots), encrypts that with the key and sends the result
//! with the token.  The server sees neither the keyphrase, nor the key, nor a save: it keeps the bytes it was sent
//! under the SHA-256 of the token, and hands them back to whoever shows that token.  A copy of the folder (a backup,
//! a disk) names no player and opens no save.
//!
//! # On disk
//!
//! `<dir>/<3 hex>/<64 hex>`: one file per keyphrase, named by its id's hex (the first 3 digits are a folder: 4096 of
//! them, a few thousand files each at millions of keyphrases).  A file is a 64-byte head, the current version, then
//! the copy of an earlier day (the undo of a bad day: a browser that synced a lost career):
//!
//! | at | size | |
//! |---|---|---|
//! | 0 | 4 | "NFV1" |
//! | 4 | u32 | flags ([`KEPT`]) |
//! | 8 | u64 | the current version's rev (random: its ETag) |
//! | 16 | i64 | when it was saved (ms since 1970) |
//! | 24 | u32 | its length |
//! | 28 | u32 | the earlier copy's length |
//! | 32 | u64 | its rev |
//! | 40 | i64 | when it was saved (0: there is none) |
//! | 48 | i64 | when the keyphrase was first saved |
//! | 56 | 8 | 0 |
//!
//! All little-endian.  The earlier copy is the last version saved on an earlier day (UTC) than the current one: the
//! first save of a day moves the current version there.  A file is written whole: a temporary file beside it
//! (`<id>.tmp-<pid>-<n>`, 0600), synced, then renamed over it, so a crash leaves the old file or the new one, never
//! half of one.  Its mtime is when the keyphrase was last used (saved, or read: touched at most once a day).
//!
//! # Limits
//!
//! - one version at most [`VaultOptions::max_body`] bytes (256 KB: a save folder without screenshots and covers packs
//!   into 6-14 KB, so that is dozens of profiles);
//! - new keyphrases stop once all of them take [`VaultOptions::new_until`] bytes, every write that grows a file at
//!   [`VaultOptions::max_bytes`] (counted in 4 KB blocks, as the disk takes them); what is kept stays readable;
//! - what the saves add to the disk comes at most [`VaultOptions::per_day`] bytes a day from all clients together, a
//!   quarter of that at once: what fills the room for new keyphrases in [`FILL_DAYS`], 2000 new keyphrases' worth at
//!   least.  A new keyphrase counts as [`VaultOptions::base`] at least (32 KB: two versions of a 14 KB save folder)
//!   and may grow to that without counting again; a save that grows its file past it counts what it adds.  So no
//!   byte on the disk goes uncounted, also of small keyphrases grown later, and a save that keeps its size, or stays
//!   within `base`, never waits for it.  The per-client limits alone let anyone with enough addresses fill the room
//!   within days, and whatever fills it stays two years (the third security audit, 2026-10-23); so a flood now takes
//!   a month, and leaves a line a minute in the log.  (The first cap counted keyphrases, each as the most it can hold,
//!   528 KB: 50 GB took 2838 a day, and turned players away at 4,300 at once, 2026-10-23.);
//! - every write that grows a file stops while the disk under the folder has less than [`VaultOptions::min_free`]
//!   bytes free, or fewer than [`VaultOptions::min_free_files`] files (inodes): the caps count bytes, the room given
//!   may be more than the disk has, and many small files run a disk out of inodes before its bytes;
//! - a keyphrase unused for [`VaultOptions::unused_ms`] (two years) goes at the next sweep, unless its flags say
//!   [`KEPT`] (an operator's choice for one keyphrase: kept however long it goes unused).
//!
//! The totals behind the caps are counted by [`Vault::sweep`] (at the start, then daily) and kept up to date by each
//! write; until the first count ends only the day's bytes and the free disk limit them.  A write during a
//! sweep may be missed by its count: the next one sets it right; a file that goes while the sweep looks at it (a
//! save's rename, a delete) is passed over.
//!
//! # The launch copy
//!
//! A vault may start as a copy of another server's (Underground 2's at its new address starts as a copy of the one at
//! its old address, made just before the launch).  Whoever makes the copy writes the time it was made into
//! `<dir>/launch-copy` (ms since 1970, decimal).  A keyphrase whose current version was saved by then came with the
//! copy and was not written since: [`Vault::is_launch_copy`] says so, and its first save here ends it (the save's time
//! is later).  No file: no keyphrase is one.
//!
//! Every call blocks (file I/O): the server makes them on its blocking threads.  Calls for one keyphrase take turns
//! (a lock per id, of 256 stripes); the others run side by side.

use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use parking_lot::{Mutex, MutexGuard};
use sha2::{Digest, Sha256};

pub mod covers;

pub const MAGIC: &[u8; 4] = b"NFV1";
pub const HEAD: usize = 64;
/// a flag: the keyphrase is kept without end (the sweep leaves it however long it goes unused)
pub const KEPT: u32 = 1;
pub const DAY_MS: i64 = 86_400_000;
/// the disk's block: what a file takes is its length rounded up to it
pub const BLOCK: u64 = 4096;
/// a temporary file that was never renamed (the server stopped mid-write) goes at the first sweep an hour after
const STALE_TMP_MS: i64 = 3_600_000;
/// the days the saves take at the least to fill the room for new keyphrases (`sized`)
pub const FILL_DAYS: u64 = 30;
/// the file in the vault's folder that says when the vault was copied from another server's (the module's doc)
pub const LAUNCH_COPY: &str = "launch-copy";

pub struct VaultOptions {
    /// the folder (made 0700)
    pub dir: PathBuf,
    /// one version's bytes at most
    pub max_body: usize,
    /// every write that grows a file stops at this many bytes in all
    pub max_bytes: u64,
    /// new keyphrases stop at this many bytes in all
    pub new_until: u64,
    /// the bytes the saves may add to the disk a day, all clients together (a quarter of it at once)
    pub per_day: u64,
    /// what a new keyphrase counts as at the least: its file grows to it without counting again
    pub base: u64,
    /// every write that grows a file stops while the disk has fewer bytes free than this...
    pub min_free: u64,
    /// ...or fewer files (inodes)
    pub min_free_files: u64,
    /// a keyphrase unused this long goes (but a [`KEPT`] one)
    pub unused_ms: i64,
}

impl VaultOptions {
    /// `gb` gigabytes in all, new keyphrases until 90 % of it, the saves adding at most what fills that in
    /// [`FILL_DAYS`], 256 KB a version, a new keyphrase 32 KB at the least, 2 GB and 100,000 files kept free on the
    /// disk, two years unused
    pub fn sized(dir: PathBuf, gb: f64) -> VaultOptions {
        let max_bytes = (gb * 1e9) as u64;
        let (max_body, new_until, base) = (256 << 10, max_bytes / 10 * 9, 32 << 10);
        VaultOptions {
            dir,
            max_body: max_body as usize,
            max_bytes,
            new_until,
            per_day: (new_until / FILL_DAYS).max(2000 * base),
            base,
            min_free: 2_000_000_000,
            min_free_files: 100_000,
            unused_ms: 730 * DAY_MS,
        }
    }
}

/// What a keyphrase is kept under: the SHA-256 of the token the page derives from it.
pub type Id = [u8; 32];

pub fn id_of(token: &[u8]) -> Id {
    Sha256::digest(token).into()
}

fn hex(id: &Id) -> String {
    id.iter().map(|b| format!("{b:02x}")).collect()
}

/// a file name that is an id's hex (64 lowercase hex digits)
fn parse_hex(name: &str) -> Option<Id> {
    let b = name.as_bytes();
    if b.len() != 64 || !b.iter().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(c)) {
        return None;
    }
    let mut id = [0u8; 32];
    for (i, o) in id.iter_mut().enumerate() {
        *o = u8::from_str_radix(&name[i * 2..i * 2 + 2], 16).ok()?;
    }
    Some(id)
}

/// what a file of `len` bytes takes on the disk
pub fn disk(len: u64) -> u64 {
    len.div_ceil(BLOCK) * BLOCK
}

/// A version: its rev (the ETag) and when it was saved.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Version {
    pub rev: u64,
    pub saved: i64,
}

/// What is known of a keyphrase's file: its current version, the copy of an earlier day, its flags, its first save.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Meta {
    pub cur: Version,
    pub prev: Option<Version>,
    pub flags: u32,
    pub created: i64,
}

/// Which version a read wants.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Want {
    Current,
    Earlier,
}

/// What a write expects to find: no keyphrase yet (If-None-Match: *), or this rev as the current version (If-Match).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Cond {
    New,
    Rev(u64),
}

/// What a write did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Put {
    Saved {
        meta: Meta,
        created: bool,
    },
    /// the condition did not hold: what is there (None: no keyphrase)
    Conflict(Option<Meta>),
    TooLarge,
    /// the caps: no new keyphrase, or no more bytes
    Full,
    /// the disk is nearly full (`min_free`, `min_free_files`): nothing that grows a file
    DiskLow,
    /// the day's bytes are taken (`per_day`): a new keyphrase, or a save that grows one, later
    Busy,
}

/// What a sweep found, and did.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Sweep {
    pub keyphrases: u64,
    pub bytes: u64,
    /// keyphrases removed after going unused
    pub removed: u64,
    /// temporary files of unfinished writes removed
    pub temps: u64,
}

pub struct Vault {
    o: VaultOptions,
    locks: Vec<Mutex<()>>,
    bytes: AtomicI64,
    count: AtomicI64,
    counted: AtomicBool,
    seq: AtomicU64,
    /// the bytes the saves may add now (`per_day`'s bucket) and when it was last filled up (ms)
    room: Mutex<(f64, i64)>,
    /// when the vault was copied from another server's ([`LAUNCH_COPY`]), if it was
    launch_copy: Option<i64>,
}

fn u32_at(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(b[i..i + 4].try_into().expect("4 bytes"))
}
fn u64_at(b: &[u8], i: usize) -> u64 {
    u64::from_le_bytes(b[i..i + 8].try_into().expect("8 bytes"))
}

/// the head of a file: its Meta and the two lengths, or None when it is not one of ours (`size`: the whole file's)
fn parse_head(b: &[u8], size: u64) -> Option<(Meta, usize, usize)> {
    if b.len() < HEAD || &b[..4] != MAGIC {
        return None;
    }
    let (len, plen) = (u32_at(b, 24) as usize, u32_at(b, 28) as usize);
    if (HEAD + len + plen) as u64 != size {
        return None;
    }
    let prev_saved = u64_at(b, 40) as i64;
    let meta = Meta {
        cur: Version { rev: u64_at(b, 8), saved: u64_at(b, 16) as i64 },
        prev: (prev_saved != 0).then(|| Version { rev: u64_at(b, 32), saved: prev_saved }),
        flags: u32_at(b, 4),
        created: u64_at(b, 48) as i64,
    };
    Some((meta, len, plen))
}

/// a whole file: the head, the current version, the earlier copy
fn file_bytes(meta: &Meta, cur: &[u8], prev: &[u8]) -> Vec<u8> {
    let mut b = Vec::with_capacity(HEAD + cur.len() + prev.len());
    b.extend_from_slice(MAGIC);
    b.extend_from_slice(&meta.flags.to_le_bytes());
    b.extend_from_slice(&meta.cur.rev.to_le_bytes());
    b.extend_from_slice(&(meta.cur.saved as u64).to_le_bytes());
    b.extend_from_slice(&(cur.len() as u32).to_le_bytes());
    b.extend_from_slice(&(prev.len() as u32).to_le_bytes());
    let p = meta.prev.unwrap_or(Version { rev: 0, saved: 0 });
    b.extend_from_slice(&p.rev.to_le_bytes());
    b.extend_from_slice(&(p.saved as u64).to_le_bytes());
    b.extend_from_slice(&(meta.created as u64).to_le_bytes());
    b.extend_from_slice(&[0u8; 8]);
    b.extend_from_slice(cur);
    b.extend_from_slice(prev);
    b
}

/// (no path in it: the console never names a keyphrase's file, nor its id)
fn corrupt() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "a keyphrase's file is damaged")
}

fn mtime_ms(m: &fs::Metadata) -> i64 {
    m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_millis() as i64)
}

fn system_time(ms: i64) -> SystemTime {
    UNIX_EPOCH + Duration::from_millis(ms.max(0) as u64)
}

/// the disk under `dir` has `min_free` bytes and `min_files` files free (what an unprivileged writer may take); true
/// when statvfs cannot tell, or the filesystem counts no files
fn disk_free(dir: &Path, min_free: u64, min_files: u64) -> bool {
    use std::os::unix::ffi::OsStrExt;
    let Ok(path) = std::ffi::CString::new(dir.as_os_str().as_bytes()) else { return true };
    // SAFETY: statvfs writes the struct it is given, and only that; the path is NUL-terminated
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(path.as_ptr(), &mut s) } != 0 {
        return true;
    }
    let free = s.f_bavail as u64 * s.f_frsize as u64;
    free >= min_free && (s.f_files == 0 || s.f_favail as u64 >= min_files)
}

fn mkdir(p: &Path) -> io::Result<()> {
    use std::os::unix::fs::DirBuilderExt;
    fs::DirBuilder::new().recursive(true).mode(0o700).create(p)
}

impl Vault {
    /// The vault in `o.dir` (made 0700 when missing).  Nothing is counted yet: [`Vault::sweep`] does that.
    pub fn open(o: VaultOptions) -> io::Result<Vault> {
        mkdir(&o.dir)?;
        let launch_copy = match fs::read_to_string(o.dir.join(LAUNCH_COPY)) {
            Ok(t) => match t.trim().parse::<i64>() {
                Ok(ms) if ms > 0 => Some(ms),
                _ => return Err(io::Error::new(io::ErrorKind::InvalidData, format!("{LAUNCH_COPY}: not a time (ms since 1970)"))),
            },
            Err(e) if e.kind() == io::ErrorKind::NotFound => None,
            Err(e) => return Err(e),
        };
        Ok(Vault {
            launch_copy,
            locks: (0..256).map(|_| Mutex::new(())).collect(),
            bytes: AtomicI64::new(0),
            count: AtomicI64::new(0),
            counted: AtomicBool::new(false),
            seq: AtomicU64::new(0),
            room: Mutex::new((Self::room_burst(&o), 0)),
            o,
        })
    }

    /// the bytes the saves may add at once: a quarter of a day's
    fn room_burst(o: &VaultOptions) -> f64 {
        o.per_day as f64 / 4.0
    }

    /// `n` bytes out of the day's, if they are left (the bucket refills at per_day a day)
    fn take_room(&self, n: u64, now: i64) -> bool {
        let mut q = self.room.lock();
        let (v, t) = *q;
        let refill = if t == 0 { 0.0 } else { (now - t).max(0) as f64 * self.o.per_day as f64 / DAY_MS as f64 };
        let v = (v + refill).min(Self::room_burst(&self.o));
        let ok = v >= n as f64;
        *q = (if ok { v - n as f64 } else { v }, now.max(t));
        ok
    }

    /// the disk under the folder has `min_free` bytes and `min_free_files` files free (what an unprivileged writer
    /// may take); true when statvfs cannot tell, or the filesystem counts no files
    fn disk_ok(&self) -> bool {
        disk_free(&self.o.dir, self.o.min_free, self.o.min_free_files)
    }

    /// when the vault was copied from another server's (the module's doc, "The launch copy"), if it was
    pub fn launch_copy(&self) -> Option<i64> {
        self.launch_copy
    }

    /// the keyphrase came with the launch copy and was not saved here since
    pub fn is_launch_copy(&self, m: &Meta) -> bool {
        self.launch_copy.is_some_and(|t| m.cur.saved <= t)
    }

    pub fn options(&self) -> &VaultOptions {
        &self.o
    }

    /// keyphrases kept, the bytes they take on the disk, and whether that was counted yet
    pub fn usage(&self) -> (u64, u64, bool) {
        (
            self.count.load(Ordering::Relaxed).max(0) as u64,
            self.bytes.load(Ordering::Relaxed).max(0) as u64,
            self.counted.load(Ordering::Relaxed),
        )
    }

    fn lock(&self, id: &Id) -> MutexGuard<'_, ()> {
        self.locks[id[0] as usize].lock()
    }

    fn path(&self, id: &Id) -> PathBuf {
        let h = hex(id);
        self.o.dir.join(&h[..3]).join(h)
    }

    /// the file's bytes, or None when there is none
    fn read(p: &Path) -> io::Result<Option<Vec<u8>>> {
        match fs::read(p) {
            Ok(b) => Ok(Some(b)),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e),
        }
    }

    /// the keyphrase was used: its file's mtime moves to now when it is a day old or more
    fn touch(p: &Path, now: i64) {
        if let Ok(m) = fs::metadata(p)
            && mtime_ms(&m) <= now - DAY_MS
            && let Ok(f) = fs::File::options().write(true).open(p)
        {
            let _ = f.set_modified(system_time(now));
        }
    }

    /// the whole file in place of the old one: a temporary file beside it (0600), synced, renamed over it
    fn write(&self, p: &Path, bytes: &[u8]) -> io::Result<()> {
        use std::os::unix::fs::OpenOptionsExt;
        let dir = p.parent().expect("a keyphrase file is in a folder");
        mkdir(dir)?;
        let tmp = PathBuf::from(format!("{}.tmp-{}-{}", p.display(), std::process::id(), self.seq.fetch_add(1, Ordering::Relaxed)));
        let done = (|| {
            let mut f = fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&tmp)?;
            f.write_all(bytes)?;
            f.sync_data()?;
            drop(f);
            fs::rename(&tmp, p)
        })();
        if done.is_err() {
            let _ = fs::remove_file(&tmp);
        }
        done
    }

    /// What is known of a keyphrase (its file's head), or None; a read: the keyphrase counts as used.
    pub fn meta(&self, id: &Id, now: i64) -> io::Result<Option<Meta>> {
        let p = self.path(id);
        let _g = self.lock(id);
        let mut f = match fs::File::open(&p) {
            Ok(f) => f,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(e),
        };
        let size = f.metadata()?.len();
        let mut head = [0u8; HEAD];
        f.read_exact(&mut head).map_err(|_| corrupt())?;
        let (meta, ..) = parse_head(&head, size).ok_or_else(corrupt)?;
        Self::touch(&p, now);
        Ok(Some(meta))
    }

    /// A keyphrase's version (the current one, or the copy of an earlier day: empty when there is none) with what is
    /// known of it, or None when there is no keyphrase; a read: the keyphrase counts as used.
    pub fn get(&self, id: &Id, want: Want, now: i64) -> io::Result<Option<(Meta, Vec<u8>)>> {
        let p = self.path(id);
        let _g = self.lock(id);
        let Some(b) = Self::read(&p)? else { return Ok(None) };
        let (meta, len, _) = parse_head(&b, b.len() as u64).ok_or_else(corrupt)?;
        Self::touch(&p, now);
        let body = match want {
            Want::Current => b[HEAD..HEAD + len].to_vec(),
            Want::Earlier => b[HEAD + len..].to_vec(),
        };
        Ok(Some((meta, body)))
    }

    /// Saves `body` as the keyphrase's current version, if `cond` holds and the caps allow.  The first save of a day
    /// keeps the version it replaces as the earlier copy.
    pub fn put(&self, id: &Id, cond: Cond, body: &[u8], now: i64) -> io::Result<Put> {
        if body.len() > self.o.max_body {
            return Ok(Put::TooLarge);
        }
        let p = self.path(id);
        let _g = self.lock(id);
        let old = Self::read(&p)?;
        let parsed = match &old {
            Some(b) => Some(parse_head(b, b.len() as u64).ok_or_else(corrupt)?),
            None => None,
        };
        match (cond, &parsed) {
            (Cond::New, Some((m, ..))) => return Ok(Put::Conflict(Some(*m))),
            (Cond::Rev(_), None) => return Ok(Put::Conflict(None)),
            (Cond::Rev(r), Some((m, ..))) if m.cur.rev != r => return Ok(Put::Conflict(Some(*m))),
            _ => {}
        }
        let (meta, prev_body): (Meta, &[u8]) = match (&old, &parsed) {
            (Some(b), Some((m, len, _))) => {
                let new_day = m.cur.saved.div_euclid(DAY_MS) != now.div_euclid(DAY_MS);
                let (prev, prev_body) = if new_day { (Some(m.cur), &b[HEAD..HEAD + len]) } else { (m.prev, &b[HEAD + len..]) };
                let rev = Self::new_rev(&[m.cur.rev, prev.map_or(0, |v| v.rev)]);
                (Meta { cur: Version { rev, saved: now }, prev, flags: m.flags, created: m.created }, prev_body)
            }
            _ => (Meta { cur: Version { rev: Self::new_rev(&[]), saved: now }, prev: None, flags: 0, created: now }, &[]),
        };
        let bytes = file_bytes(&meta, body, prev_body);
        let (was, will) = (old.as_ref().map(|b| disk(b.len() as u64)), disk(bytes.len() as u64));
        let grows = will as i64 - was.unwrap_or(0) as i64;
        // what counts against the day's bytes: a new keyphrase at least `base`, a save what it grows the file past
        // both its blocks and `base`
        let counts = match was {
            None => will.max(self.o.base),
            Some(was) => will.saturating_sub(was.max(self.o.base)),
        };
        if self.counted.load(Ordering::Relaxed) {
            let total = self.bytes.load(Ordering::Relaxed);
            let over = |cap: u64| total + grows > cap as i64;
            if (old.is_none() && over(self.o.new_until)) || (grows > 0 && over(self.o.max_bytes)) {
                return Ok(Put::Full);
            }
        }
        if grows > 0 && !self.disk_ok() {
            return Ok(Put::DiskLow);
        }
        if counts > 0 && !self.take_room(counts, now) {
            return Ok(Put::Busy);
        }
        self.write(&p, &bytes)?;
        self.bytes.fetch_add(grows, Ordering::Relaxed);
        if old.is_none() {
            self.count.fetch_add(1, Ordering::Relaxed);
        }
        Ok(Put::Saved { meta, created: old.is_none() })
    }

    /// a rev that is none of `not` (nor 0)
    fn new_rev(not: &[u64]) -> u64 {
        loop {
            let r = nfsu2_util::rand::u64();
            if r != 0 && !not.contains(&r) {
                return r;
            }
        }
    }

    /// Removes a keyphrase, both versions: false when there was none.
    pub fn delete(&self, id: &Id) -> io::Result<bool> {
        let p = self.path(id);
        let _g = self.lock(id);
        let size = match fs::metadata(&p) {
            Ok(m) => m.len(),
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(false),
            Err(e) => return Err(e),
        };
        fs::remove_file(&p)?;
        self.bytes.fetch_sub(disk(size) as i64, Ordering::Relaxed);
        self.count.fetch_sub(1, Ordering::Relaxed);
        Ok(true)
    }

    /// Sets a keyphrase's flags (an operator's: [`KEPT`]); its versions and its last use stay as they were.  False
    /// when there is no such keyphrase.
    pub fn set_flags(&self, id: &Id, flags: u32) -> io::Result<bool> {
        let p = self.path(id);
        let _g = self.lock(id);
        let Some(b) = Self::read(&p)? else { return Ok(false) };
        let (mut meta, len, _) = parse_head(&b, b.len() as u64).ok_or_else(corrupt)?;
        let used = fs::metadata(&p)?.modified()?;
        meta.flags = flags;
        self.write(&p, &file_bytes(&meta, &b[HEAD..HEAD + len], &b[HEAD + len..]))?;
        fs::File::options().write(true).open(&p)?.set_modified(used)?;
        Ok(true)
    }

    /// Goes through every file: removes the keyphrases unused for `unused_ms` (but the [`KEPT`] ones) and the
    /// temporary files of writes that never finished, and counts the rest, which the caps go by from then on.  Files
    /// that are not a keyphrase's are left alone and not counted.
    pub fn sweep(&self, now: i64) -> io::Result<Sweep> {
        let mut s = Sweep::default();
        let shards = match fs::read_dir(&self.o.dir) {
            Ok(r) => r,
            Err(e) if e.kind() == io::ErrorKind::NotFound => {
                self.set_totals(&s);
                return Ok(s);
            }
            Err(e) => return Err(e),
        };
        for shard in shards {
            let shard = shard?;
            let name = shard.file_name();
            let name = name.to_string_lossy();
            if name.len() != 3 || !shard.file_type()?.is_dir() {
                continue;
            }
            let files = match fs::read_dir(shard.path()) {
                Ok(r) => r,
                Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
                Err(e) => return Err(e),
            };
            for f in files {
                let f = f?;
                // a file that went since it was listed (a save's temporary file renamed, a delete): passed over, not
                // the end of the count (which left the caps off until the next day's)
                let m = match f.metadata() {
                    Ok(m) => m,
                    Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
                    Err(e) => return Err(e),
                };
                let name = f.file_name().to_string_lossy().into_owned();
                if !m.is_file() {
                    continue;
                }
                if name.contains(".tmp-") && parse_hex(&name[..name.find(".tmp-").unwrap_or(0)]).is_some() {
                    if mtime_ms(&m) < now - STALE_TMP_MS && fs::remove_file(f.path()).is_ok() {
                        s.temps += 1;
                    } else {
                        s.bytes += disk(m.len());
                    }
                    continue;
                }
                let Some(id) = parse_hex(&name) else { continue };
                if mtime_ms(&m) < now - self.o.unused_ms && self.remove_unused(&id, now)? {
                    s.removed += 1;
                    continue;
                }
                s.keyphrases += 1;
                s.bytes += disk(m.len());
            }
        }
        self.set_totals(&s);
        Ok(s)
    }

    /// removes a keyphrase that is still unused once its lock is held (a save may have come since it was seen), and
    /// not [`KEPT`]
    fn remove_unused(&self, id: &Id, now: i64) -> io::Result<bool> {
        let p = self.path(id);
        let _g = self.lock(id);
        let Ok(m) = fs::metadata(&p) else { return Ok(false) };
        if mtime_ms(&m) >= now - self.o.unused_ms {
            return Ok(false);
        }
        let mut head = [0u8; HEAD];
        let kept = fs::File::open(&p).and_then(|mut f| f.read_exact(&mut head)).is_ok()
            && parse_head(&head, m.len()).is_some_and(|(meta, ..)| meta.flags & KEPT != 0);
        if kept {
            return Ok(false);
        }
        fs::remove_file(&p)?;
        Ok(true)
    }

    fn set_totals(&self, s: &Sweep) {
        self.bytes.store(s.bytes as i64, Ordering::Relaxed);
        self.count.store(s.keyphrases as i64, Ordering::Relaxed);
        self.counted.store(true, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests;
