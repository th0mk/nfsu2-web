//! The hand-over store (`/covers`): a player's covers, kept once for the move from Underground 2's old address to its
//! new one (the server's `app/covers.rs`).
//!
//! The DVD and magazine covers of a save folder (a cover shoot's file is 1,048,612 bytes, about 500 KB packed) are too
//! big for a keyphrase's box (256 KB), so the old address's page seals them like a box, under a token of the move's own
//! (the page derives it from the move's phrase with info of its own: never a keyphrase's), and writes them here once;
//! the new address's page reads them, and deletes them once it took them in.  An entry nobody takes goes after
//! [`KEEP_MS`] (30 days).  As with the vault, the server sees no cover and no phrase: it keeps the bytes it was sent
//! under the SHA-256 of the token.
//!
//! # On disk
//!
//! `<dir>/<64 hex>`: one file per entry, a 16-byte head ("NFC1", when it was stored in ms since 1970 as an i64, the
//! length as a u32, 0) and the bytes.  A file is written whole beside its name (`<hex>.tmp-<pid>-<n>`, 0600, synced)
//! and linked to it: an entry is written once, and a second write finds it there.  `<dir>/counts` keeps what the
//! store did (stored, imported, expired, refused: [`Counts`]) across restarts.
//!
//! # Limits
//!
//! - an entry at most [`CoverOptions::max_entry`] bytes;
//! - all entries at most [`CoverOptions::max_bytes`] (counted in 4 KB blocks, as the disk takes them);
//! - no write while the disk under the folder has less than [`CoverOptions::min_free`] bytes, or
//!   [`CoverOptions::min_free_files`] files, free.
//!
//! A refusal is the page's cue to offer the covers as a download instead.  Every call blocks (file I/O): the server
//! makes them on its blocking threads.

use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicI64, AtomicU64, Ordering};

use parking_lot::Mutex;

use super::{DAY_MS, Id, disk, disk_free, hex, mkdir, mtime_ms, parse_hex};

pub const MAGIC: &[u8; 4] = b"NFC1";
pub const HEAD: usize = 16;
/// an entry nobody took goes after this long
pub const KEEP_MS: i64 = 30 * DAY_MS;
/// what the store did, kept across restarts
pub const COUNTS: &str = "counts";
/// a temporary file that was never linked (the server stopped mid-write) goes at the first sweep an hour after
const STALE_TMP_MS: i64 = 3_600_000;

pub struct CoverOptions {
    /// the folder (made 0700)
    pub dir: PathBuf,
    /// one entry's bytes at most
    pub max_entry: usize,
    /// all entries' bytes (on the disk) at most
    pub max_bytes: u64,
    /// no write while the disk has fewer bytes free than this...
    pub min_free: u64,
    /// ...or fewer files (inodes)
    pub min_free_files: u64,
}

impl CoverOptions {
    /// The defaults: 64 MB an entry (128 covers of about 500 KB packed: every cover shoot of several careers; the most
    /// one save folder can hold is not measured yet), 50 GB in all, 2 GB and 100,000 files kept free on the disk.
    pub fn defaults(dir: PathBuf) -> CoverOptions {
        CoverOptions { dir, max_entry: 64 << 20, max_bytes: 50_000_000_000, min_free: 2_000_000_000, min_free_files: 100_000 }
    }
}

/// What the store did since it was first opened (kept across restarts in `<dir>/counts`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counts {
    /// entries written
    pub stored: u64,
    /// entries the new page took in (deleted by it)
    pub imported: u64,
    /// entries nobody took, gone after KEEP_MS
    pub expired: u64,
    /// writes refused (too large, full, no room on the disk, there already, too many from one client)
    pub refused: u64,
}

/// What a write did.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Store {
    Stored,
    /// an entry under that token is there already (written once)
    Exists,
    TooLarge,
    /// all entries take max_bytes
    Full,
    /// the disk is nearly full (`min_free`, `min_free_files`)
    DiskLow,
}

/// What a sweep found, and did.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CoverSweep {
    pub entries: u64,
    pub bytes: u64,
    /// entries gone after KEEP_MS
    pub expired: u64,
    /// temporary files of unfinished writes removed
    pub temps: u64,
}

pub struct CoverStore {
    o: CoverOptions,
    bytes: AtomicI64,
    count: AtomicI64,
    counts: Mutex<Counts>,
    seq: AtomicU64,
}

/// (no path in it: the console never names an entry's file)
fn corrupt() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "a hand-over entry's file is damaged")
}

/// an entry's head: when it was stored, if the file is one of ours of this length
fn parse_head(b: &[u8], size: u64) -> Option<i64> {
    if b.len() < HEAD || &b[..4] != MAGIC {
        return None;
    }
    let len = u32::from_le_bytes(b[12..16].try_into().ok()?) as u64;
    (HEAD as u64 + len == size).then(|| i64::from_le_bytes(b[4..12].try_into().expect("8 bytes")))
}

impl CoverStore {
    /// The store in `o.dir` (made 0700 when missing), its counts read back; its entries are counted by
    /// [`CoverStore::sweep`] (until then the caps count from 0).
    pub fn open(o: CoverOptions) -> io::Result<CoverStore> {
        mkdir(&o.dir)?;
        let counts = match fs::read_to_string(o.dir.join(COUNTS)) {
            Ok(t) => {
                let n: Vec<u64> = t.split_whitespace().filter_map(|w| w.parse().ok()).collect();
                match n[..] {
                    [stored, imported, expired, refused] => Counts { stored, imported, expired, refused },
                    _ => Counts::default(),
                }
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => Counts::default(),
            Err(e) => return Err(e),
        };
        Ok(CoverStore { o, bytes: AtomicI64::new(0), count: AtomicI64::new(0), counts: Mutex::new(counts), seq: AtomicU64::new(0) })
    }

    pub fn options(&self) -> &CoverOptions {
        &self.o
    }

    /// entries kept and the bytes they take on the disk
    pub fn usage(&self) -> (u64, u64) {
        (self.count.load(Ordering::Relaxed).max(0) as u64, self.bytes.load(Ordering::Relaxed).max(0) as u64)
    }

    /// what the store did, all told
    pub fn counts(&self) -> Counts {
        *self.counts.lock()
    }

    /// a write refused before it reached the store (the server's limits per client): counted with the store's own
    pub fn refused(&self) {
        self.count_and_keep(|c| c.refused += 1);
    }

    /// counts changed, and kept (a temporary file, renamed over the old one; a failure costs only the counts)
    fn count_and_keep(&self, f: impl FnOnce(&mut Counts)) {
        let mut c = self.counts.lock();
        f(&mut c);
        let text = format!("{} {} {} {}\n", c.stored, c.imported, c.expired, c.refused);
        let tmp = self.o.dir.join(format!("{COUNTS}.tmp-{}", std::process::id()));
        if fs::write(&tmp, text).and_then(|_| fs::rename(&tmp, self.o.dir.join(COUNTS))).is_err() {
            let _ = fs::remove_file(&tmp);
        }
    }

    fn path(&self, id: &Id) -> PathBuf {
        self.o.dir.join(hex(id))
    }

    /// Keeps `body` under `id`, once: [`Store::Exists`] when an entry is there (one past KEEP_MS is replaced).
    pub fn put(&self, id: &Id, body: &[u8], now: i64) -> io::Result<Store> {
        let refuse = |s: Store| {
            self.count_and_keep(|c| c.refused += 1);
            Ok(s)
        };
        if body.len() > self.o.max_entry {
            return refuse(Store::TooLarge);
        }
        let size = disk((HEAD + body.len()) as u64);
        // the room, taken before the write (and given back when it does not happen): two writes at once never both
        // fit into the last of it
        let before = self.bytes.fetch_add(size as i64, Ordering::SeqCst);
        if before.max(0) as u64 + size > self.o.max_bytes {
            self.bytes.fetch_sub(size as i64, Ordering::SeqCst);
            return refuse(Store::Full);
        }
        if !disk_free(&self.o.dir, self.o.min_free, self.o.min_free_files) {
            self.bytes.fetch_sub(size as i64, Ordering::SeqCst);
            return refuse(Store::DiskLow);
        }
        let done = self.link_new(id, body, now);
        match done {
            Ok(Store::Stored) => {
                self.count.fetch_add(1, Ordering::SeqCst);
                self.count_and_keep(|c| c.stored += 1);
                Ok(Store::Stored)
            }
            Ok(other) => {
                self.bytes.fetch_sub(size as i64, Ordering::SeqCst);
                refuse(other)
            }
            Err(e) => {
                self.bytes.fetch_sub(size as i64, Ordering::SeqCst);
                Err(e)
            }
        }
    }

    /// the file written beside its name and linked to it: Stored, or Exists when an entry was there first
    fn link_new(&self, id: &Id, body: &[u8], now: i64) -> io::Result<Store> {
        use std::os::unix::fs::OpenOptionsExt;
        let p = self.path(id);
        let tmp = PathBuf::from(format!("{}.tmp-{}-{}", p.display(), std::process::id(), self.seq.fetch_add(1, Ordering::Relaxed)));
        let written = (|| {
            let mut f = fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&tmp)?;
            let mut head = Vec::with_capacity(HEAD + body.len());
            head.extend_from_slice(MAGIC);
            head.extend_from_slice(&now.to_le_bytes());
            head.extend_from_slice(&(body.len() as u32).to_le_bytes());
            head.extend_from_slice(body);
            f.write_all(&head)?;
            f.sync_data()
        })();
        let linked = written.and_then(|_| match fs::hard_link(&tmp, &p) {
            Ok(()) => Ok(Store::Stored),
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {
                // one nobody took in time goes (the sweep had not come to it yet), and this one takes its place
                if self.stored_at(&p)?.is_some_and(|t| now - t >= KEEP_MS) && self.remove(&p, true)? {
                    fs::hard_link(&tmp, &p)
                        .map(|_| Store::Stored)
                        .or_else(|e| if e.kind() == io::ErrorKind::AlreadyExists { Ok(Store::Exists) } else { Err(e) })
                } else {
                    Ok(Store::Exists)
                }
            }
            Err(e) => Err(e),
        });
        let _ = fs::remove_file(&tmp);
        linked
    }

    /// when the entry at p was stored (None: no entry; a file that is not one: an error)
    fn stored_at(&self, p: &Path) -> io::Result<Option<i64>> {
        let mut f = match fs::File::open(p) {
            Ok(f) => f,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(e),
        };
        let size = f.metadata()?.len();
        let mut head = [0u8; HEAD];
        f.read_exact(&mut head).map_err(|_| corrupt())?;
        parse_head(&head, size).map(Some).ok_or_else(corrupt)
    }

    /// the entry's file goes, and its bytes from the totals (`expired`: counted so); false when it was gone already
    fn remove(&self, p: &Path, expired: bool) -> io::Result<bool> {
        let size = match fs::metadata(p) {
            Ok(m) => m.len(),
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(false),
            Err(e) => return Err(e),
        };
        match fs::remove_file(p) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(false),
            Err(e) => return Err(e),
        }
        self.bytes.fetch_sub(disk(size) as i64, Ordering::SeqCst);
        self.count.fetch_sub(1, Ordering::SeqCst);
        if expired {
            self.count_and_keep(|c| c.expired += 1);
        }
        Ok(true)
    }

    /// The entry's bytes; None when there is none (or it is past KEEP_MS: it goes at the next sweep).
    pub fn get(&self, id: &Id, now: i64) -> io::Result<Option<Vec<u8>>> {
        let b = match fs::read(self.path(id)) {
            Ok(b) => b,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(e),
        };
        let at = parse_head(&b, b.len() as u64).ok_or_else(corrupt)?;
        Ok((now - at < KEEP_MS).then(|| b[HEAD..].to_vec()))
    }

    /// The entry goes, taken in by the new page (counted as imported); false when there is none.
    pub fn delete(&self, id: &Id) -> io::Result<bool> {
        let gone = self.remove(&self.path(id), false)?;
        if gone {
            self.count_and_keep(|c| c.imported += 1);
        }
        Ok(gone)
    }

    /// Counts the entries (the caps' totals), removes those past KEEP_MS and the temporary files of writes that never
    /// ended.  A file that goes while the sweep looks at it is passed over.
    pub fn sweep(&self, now: i64) -> io::Result<CoverSweep> {
        let mut s = CoverSweep::default();
        let mut expired = Vec::new();
        for f in fs::read_dir(&self.o.dir)? {
            let f = f?;
            let m = match f.metadata() {
                Ok(m) => m,
                Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
                Err(e) => return Err(e),
            };
            if !m.is_file() {
                continue;
            }
            let name = f.file_name().to_string_lossy().into_owned();
            if let Some(i) = name.find(".tmp-")
                && parse_hex(&name[..i]).is_some()
            {
                if mtime_ms(&m) < now - STALE_TMP_MS && fs::remove_file(f.path()).is_ok() {
                    s.temps += 1;
                } else {
                    s.bytes += disk(m.len());
                }
                continue;
            }
            if parse_hex(&name).is_none() {
                continue;
            }
            match self.stored_at(&f.path()) {
                Ok(Some(t)) if now - t >= KEEP_MS => expired.push(f.path()),
                Ok(Some(_)) | Err(_) => {
                    s.entries += 1;
                    s.bytes += disk(m.len());
                }
                Ok(None) => {}
            }
        }
        self.count.store(s.entries as i64, Ordering::SeqCst);
        self.bytes.store(s.bytes as i64, Ordering::SeqCst);
        for p in expired {
            // (counted in, then out again: the totals stay right while a write runs beside the sweep)
            if let Ok(m) = fs::metadata(&p) {
                self.count.fetch_add(1, Ordering::SeqCst);
                self.bytes.fetch_add(disk(m.len()) as i64, Ordering::SeqCst);
                if self.remove(&p, true)? {
                    s.expired += 1;
                } else {
                    self.count.fetch_sub(1, Ordering::SeqCst);
                    self.bytes.fetch_sub(disk(m.len()) as i64, Ordering::SeqCst);
                }
            }
        }
        Ok(s)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::id_of;

    const T0: i64 = 1_790_000_000_000;

    fn store(o: impl FnOnce(&mut CoverOptions)) -> (tempfile::TempDir, CoverStore) {
        let dir = tempfile::tempdir().expect("a temporary folder");
        let mut opts = CoverOptions::defaults(dir.path().join("covers"));
        opts.min_free = 0;
        opts.min_free_files = 0;
        o(&mut opts);
        let s = CoverStore::open(opts).expect("the store");
        (dir, s)
    }

    // Written once, read, deleted by the page that took it in; a second write finds it there; an unknown token has
    // nothing; the files name no token and are 0600; the counts survive a restart
    #[test]
    fn an_entry_written_once_read_and_taken_in() {
        let (dir, s) = store(|_| {});
        let (a, b) = (id_of(b"the move's token"), id_of(b"another"));
        assert_eq!(s.put(&a, b"covers", T0).unwrap(), Store::Stored);
        assert_eq!(s.put(&a, b"other covers", T0 + 1).unwrap(), Store::Exists);
        assert_eq!(s.get(&a, T0 + 2).unwrap().as_deref(), Some(&b"covers"[..]));
        assert_eq!(s.get(&b, T0 + 2).unwrap(), None);
        assert_eq!(s.usage(), (1, 4096));
        let names: Vec<String> =
            fs::read_dir(dir.path().join("covers")).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).collect();
        assert!(names.iter().all(|n| n == COUNTS || (n.len() == 64 && parse_hex(n).is_some())), "{names:?}");
        assert!(!names.iter().any(|n| n.contains("token")));
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(fs::metadata(dir.path().join("covers").join(hex(&a))).unwrap().permissions().mode() & 0o777, 0o600);
        assert!(s.delete(&a).unwrap());
        assert!(!s.delete(&a).unwrap());
        assert_eq!(s.get(&a, T0 + 3).unwrap(), None);
        assert_eq!(s.usage(), (0, 0));
        assert_eq!(s.counts(), Counts { stored: 1, imported: 1, expired: 0, refused: 1 });
        drop(s);
        let again = CoverStore::open(CoverOptions::defaults(dir.path().join("covers"))).unwrap();
        assert_eq!(again.counts(), Counts { stored: 1, imported: 1, expired: 0, refused: 1 });
    }

    // An entry nobody took is gone after 30 days: not read, removed by the sweep (counted expired), and a write under
    // its token takes its place; a sweep counts the entries and clears unfinished writes an hour old
    #[test]
    fn an_entry_nobody_took_goes_after_30_days() {
        let (dir, s) = store(|_| {});
        let (a, b, c) = (id_of(b"a"), id_of(b"b"), id_of(b"c"));
        s.put(&a, b"a", T0).unwrap();
        s.put(&b, b"b", T0 + DAY_MS).unwrap();
        s.put(&c, b"c", T0).unwrap();
        assert!(s.get(&a, T0 + KEEP_MS - 1).unwrap().is_some());
        assert_eq!(s.get(&a, T0 + KEEP_MS).unwrap(), None);
        assert_eq!(s.put(&c, b"c again", T0 + KEEP_MS).unwrap(), Store::Stored, "past its time: replaced");
        let tmp = dir.path().join("covers").join(format!("{}.tmp-1-1", hex(&a)));
        fs::write(&tmp, b"half").unwrap();
        fs::File::options().write(true).open(&tmp).unwrap().set_modified(crate::system_time(T0 + KEEP_MS - 2 * 3_600_000)).unwrap();
        let sw = s.sweep(T0 + KEEP_MS).unwrap();
        assert_eq!((sw.entries, sw.expired, sw.temps), (2, 1, 1), "{sw:?}");
        assert!(!tmp.exists());
        assert_eq!(s.usage(), (2, 2 * 4096));
        assert_eq!(s.get(&c, T0 + KEEP_MS).unwrap().as_deref(), Some(&b"c again"[..]));
        assert_eq!(s.counts(), Counts { stored: 4, imported: 0, expired: 2, refused: 0 });
        assert_eq!(s.sweep(T0 + DAY_MS + KEEP_MS).unwrap().expired, 1);
        assert_eq!(s.usage(), (1, 4096));
    }

    // The caps: an entry past max_entry, all entries past max_bytes (in 4 KB blocks), the disk nearly full; each
    // refusal counted, nothing written
    #[test]
    fn each_cap_and_the_budget() {
        let (_dir, s) = store(|o| {
            o.max_entry = 10_000;
            o.max_bytes = 3 * 4096;
        });
        assert_eq!(s.put(&id_of(b"big"), &[0; 10_001], T0).unwrap(), Store::TooLarge);
        assert_eq!(s.put(&id_of(b"1"), &[0; 5000], T0).unwrap(), Store::Stored);
        assert_eq!(s.put(&id_of(b"2"), &[0; 4000], T0).unwrap(), Store::Stored);
        assert_eq!(s.put(&id_of(b"3"), &[0; 4000], T0).unwrap(), Store::Full);
        assert_eq!(s.usage(), (2, 3 * 4096));
        assert!(s.delete(&id_of(b"1")).unwrap());
        assert_eq!(s.put(&id_of(b"3"), &[0; 4000], T0).unwrap(), Store::Stored);
        let (_d2, low) = store(|o| o.min_free = u64::MAX);
        assert_eq!(low.put(&id_of(b"x"), b"x", T0).unwrap(), Store::DiskLow);
        assert_eq!(low.usage(), (0, 0));
        assert_eq!(s.counts().refused, 2);
        assert_eq!(low.counts().refused, 1);
    }

    // A damaged file is an error, never another entry's bytes; the sweep counts it and goes on
    #[test]
    fn a_damaged_entry() {
        let (dir, s) = store(|_| {});
        let a = id_of(b"a");
        fs::write(dir.path().join("covers").join(hex(&a)), b"NFC1 not ours").unwrap();
        assert!(s.get(&a, T0).is_err());
        assert_eq!(s.sweep(T0).unwrap().entries, 1);
    }
}
