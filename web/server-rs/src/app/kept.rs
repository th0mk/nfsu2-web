//! The game data's blocks compressed once at brotli's best (quality 11) and kept in the state folder (`blocks/`), for
//! the /data-blocks/ answers of one block (what the page asks for).  They cross the network smaller than at the
//! quality 3 a block is compressed with as it is sent (the tracks 41 -> 33 % of their size, SPEED2.EXE 50 -> 41 %,
//! the music 86 -> 84 %; the movies do not compress either way), and every block is tried, not only those whose
//! first 64 KB compress (the answers' probe, blocks.rs: many car textures, sent as they are before), at about 100
//! times the CPU: so never while a client waits.  A block not kept yet is answered as before and queued (blocks.rs).
//! Threads of their own (NFSU2_COMPRESS_THREADS, 2 unless given, 0 with --dev; 0: the answers as before) at the lowest
//! priority make what the answers queued first, then every block of each new manifest (a changed file is a new key),
//! and remove what no file of the manifest has any more.  The lowest priority only puts them behind the server's
//! other threads on a core they share: under a CPU quota (nfsu2.slice's 14 cores, which the free-roam hub's busy cap
//! reads too) their cores count all the same, 2 of 14 on top of a busy evening's 11.8.  So they wait while the
//! quota is PAUSE_AT used and go on below RESUME_AT, and never take it to the cap's 85 % themselves.
//!
//! A kept block is a file named by a hash of the game file's real path, size, mtime and the block's number: its
//! brotli stream, or nothing at all for a block that is sent as it is: brotli makes it no smaller, or zstd at its
//! fastest makes it not even 0.5 % smaller (the movies' video: not worth brotli's time).  Written next to its place,
//! on the disk before it is renamed into it: a reader finds a whole one or none, also after a power cut (Cloudflare
//! would keep a cut-short stream for good).  The folder stops growing at MAX_BYTES, or while the disk under it has
//! less than MIN_FREE free, looked at again every ROOM_EVERY then; the answers are as before meanwhile, and the
//! queues wait.

use std::collections::{HashSet, VecDeque};
use std::io::{self, Write};
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant, SystemTime};

use parking_lot::{Condvar, Mutex};
use sha2::{Digest, Sha256};

use nfsu2_util::cpu::{QuotaBusy, QuotaUse};
use nfsu2_util::log;

use super::blocks::read_full_at;
use super::manifest::BLOCK;

/// brotli's best, with a window larger than a block (a browser takes up to 2^24); the tests take 5, as 11 takes
/// seconds a block of their made-up bytes
pub const QUALITY: u32 = if cfg!(test) { 5 } else { 11 };
pub const LGWIN: u32 = 21;
/// the folder's most, and the least the disk under it keeps free
const MAX_BYTES: u64 = 16 << 30;
const MIN_FREE: u64 = 2 << 30;
/// the most blocks the answers queue (the manifest's own are all queued, about 5000 for a game folder)
const FIRST_MAX: usize = 20_000;
/// a file being written that is this old is a crash's: the sweep removes it
const STALE_TMP: Duration = Duration::from_secs(600);
/// how often the room is looked at again while there is none (a full disk freed, an old image removed)
const ROOM_EVERY: Duration = Duration::from_millis(if cfg!(test) { 50 } else { 60_000 });
/// the CPU quota the server shares: the threads wait from this share of it used, and go on below RESUME_AT (the gap
/// is their own 2 of 14 cores, so that their pause does not end it at once); read at most every PACE_EVERY
const PAUSE_AT: f64 = 0.70;
const RESUME_AT: f64 = 0.55;
const PACE_EVERY: Duration = Duration::from_millis(if cfg!(test) { 50 } else { 5_000 });

pub type Key = [u8; 16];

/// one block of one version of a file
pub fn key(real: &str, md: &std::fs::Metadata, b: u64) -> Key {
    let mut h = Sha256::new();
    h.update(real.as_bytes());
    h.update([0]);
    h.update(md.size().to_le_bytes());
    h.update(md.mtime().to_le_bytes());
    h.update(md.mtime_nsec().to_le_bytes());
    h.update(b.to_le_bytes());
    h.finalize()[..16].try_into().expect("16 bytes")
}

fn hex(k: &Key) -> String {
    k.iter().map(|b| format!("{b:02x}")).collect()
}

/// a kept block, as a lookup finds it
pub enum Kept {
    /// its brotli stream: the open file and its length
    Br(std::fs::File, u64),
    /// brotli makes it no smaller: sent as it is
    AsIs,
    /// not kept (yet)
    No,
}

/// a block to make
struct Job {
    real: String,
    size: u64,
    mtime: (i64, i64),
    b: u64,
    key: Key,
}

#[derive(Default)]
struct Queues {
    /// what the answers queued, and their keys (a block many answers ask for is queued once)
    first: VecDeque<Job>,
    in_first: HashSet<Key>,
    /// the manifest's blocks that are not kept
    rest: VecDeque<Job>,
    /// whether the made blocks were told since the queues last ran dry
    told: bool,
    /// blocks made since the queues last ran dry
    made: u64,
}

/// the CPU quota the server shares, as the threads read it (PAUSE_AT, RESUME_AT)
struct Pace {
    /// the fullest quota's use since the last call (QuotaBusy); None: no quota to read (no Linux, no cgroup v2)
    sample: Option<Box<dyn FnMut() -> Option<QuotaUse> + Send>>,
    at: Instant,
    paused: bool,
}

pub struct KeptBlocks {
    dir: PathBuf,
    threads: usize,
    q: Mutex<Queues>,
    cv: Condvar,
    /// the bytes the folder takes (counted by each sweep, then added to)
    bytes: AtomicU64,
    /// whether there is room for more, and when that was looked at
    room: AtomicBool,
    room_at: Mutex<Instant>,
    pace: Mutex<Pace>,
    /// the sweeps started: an older one stops when a newer starts
    sweeps: AtomicU64,
    /// the manifest swept last (its ETag): the same manifest again is not swept again
    swept: Mutex<String>,
    tmp_n: AtomicU64,
}

impl KeptBlocks {
    /// the folder (made 0700 if it is not there) and `threads` threads that make the blocks
    pub fn open(dir: PathBuf, threads: usize) -> io::Result<Arc<KeptBlocks>> {
        std::fs::DirBuilder::new().recursive(true).mode(0o700).create(&dir)?;
        let k = Arc::new(KeptBlocks {
            dir,
            threads,
            q: Mutex::new(Queues { told: true, ..Default::default() }),
            cv: Condvar::new(),
            bytes: AtomicU64::new(0),
            room: AtomicBool::new(true),
            room_at: Mutex::new(Instant::now()),
            pace: Mutex::new(Pace {
                sample: QuotaBusy::find().map(|mut q| Box::new(move || q.sample_use()) as Box<dyn FnMut() -> Option<QuotaUse> + Send>),
                at: Instant::now(),
                paused: false,
            }),
            sweeps: AtomicU64::new(0),
            swept: Mutex::new(String::new()),
            tmp_n: AtomicU64::new(0),
        });
        k.check_room();
        for i in 0..threads {
            let k = k.clone();
            std::thread::Builder::new().name(format!("nfsu2-kept-{i}")).spawn(move || {
                nfsu2_util::cpu::lower_this_thread(19);
                k.work();
            })?;
        }
        Ok(k)
    }

    fn path(&self, k: &Key) -> PathBuf {
        let h = hex(k);
        self.dir.join(&h[..2]).join(h)
    }

    /// the kept block of this key (blocking: a file opened)
    pub fn lookup(&self, k: &Key) -> Kept {
        let Ok(f) = std::fs::File::open(self.path(k)) else { return Kept::No };
        match f.metadata() {
            Ok(m) if m.len() == 0 => Kept::AsIs,
            Ok(m) => Kept::Br(f, m.len()),
            Err(_) => Kept::No,
        }
    }

    /// whether blocks are kept (false while the folder or the disk is full: the answers are as before then).  Without
    /// room no block is made, so the room is looked at again here, every ROOM_EVERY: one look at a moment the disk
    /// was short (an image pulled at a release) kept them off until a restart.
    pub fn keeping(&self) -> bool {
        if !self.room.load(Ordering::Relaxed) && self.room_at.lock().elapsed() >= ROOM_EVERY {
            self.check_room();
        }
        self.room.load(Ordering::Relaxed)
    }

    /// whether the CPU quota the server shares is too full to compress now (PAUSE_AT, RESUME_AT), read at most every
    /// PACE_EVERY; a change is told on the console
    fn slice_full(&self) -> bool {
        let mut p = self.pace.lock();
        if p.at.elapsed() < PACE_EVERY {
            return p.paused;
        }
        p.at = Instant::now();
        let Some(u) = p.sample.as_mut().and_then(|f| f()) else { return p.paused };
        let (was, share) = (p.paused, u.share());
        p.paused = share >= if was { RESUME_AT } else { PAUSE_AT };
        if p.paused != was {
            log::line(&format!(
                "kept blocks: {} while {} uses {:.0} % of its {} cores",
                if p.paused { "waiting" } else { "going on" },
                u.group,
                share * 100.0,
                u.cores
            ));
        }
        p.paused
    }

    /// a block an answer did not find, to be made before the manifest's others
    pub fn queue(&self, real: &str, md: &std::fs::Metadata, b: u64) {
        if !self.keeping() {
            return;
        }
        let k = key(real, md, b);
        let mut q = self.q.lock();
        if q.first.len() < FIRST_MAX && q.in_first.insert(k) {
            q.first.push_back(Job { real: real.to_string(), size: md.size(), mtime: (md.mtime(), md.mtime_nsec()), b, key: k });
            self.cv.notify_one();
        }
    }

    /// The manifest's files (each game file's real path and metadata, as `locate` finds it from its path in the
    /// manifest): every block not kept is queued, and what is kept for no block of theirs is removed.  On a thread
    /// of its own; nothing when this manifest (`etag`) was swept already.
    pub fn sweep(
        self: &Arc<Self>,
        etag: &str,
        paths: Vec<String>,
        locate: impl Fn(&str) -> Option<(String, std::fs::Metadata)> + Send + 'static,
    ) {
        {
            let mut s = self.swept.lock();
            if *s == etag {
                return;
            }
            *s = etag.to_string();
        }
        let n = self.sweeps.fetch_add(1, Ordering::SeqCst) + 1;
        let (k, etag) = (self.clone(), etag.to_string());
        let _ = std::thread::Builder::new().name("nfsu2-kept-sweep".into()).spawn(move || {
            nfsu2_util::cpu::lower_this_thread(19);
            if let Err(e) = k.sweep_now(n, &paths, &locate) {
                log::error(&format!("kept blocks: {}: {e}", k.dir.display()));
                // not swept: the next look at the manifest (a ?refresh) sweeps it again
                let mut s = k.swept.lock();
                if *s == etag {
                    s.clear();
                }
            }
        });
    }

    fn sweep_now(&self, n: u64, paths: &[String], locate: &dyn Fn(&str) -> Option<(String, std::fs::Metadata)>) -> io::Result<()> {
        let stale = || self.sweeps.load(Ordering::SeqCst) != n;
        let (mut live, mut rest) = (HashSet::new(), VecDeque::new());
        for p in paths {
            if stale() {
                return Ok(());
            }
            let Some((real, md)) = locate(p) else { continue };
            for b in 0..md.size().div_ceil(BLOCK) {
                let k = key(&real, &md, b);
                live.insert(k);
                if !self.path(&k).exists() {
                    rest.push_back(Job { real: real.clone(), size: md.size(), mtime: (md.mtime(), md.mtime_nsec()), b, key: k });
                }
            }
        }
        // what no block of the manifest has, and files a crash left half written
        let names: HashSet<String> = live.iter().map(hex).collect();
        let mut bytes = 0;
        for sub in std::fs::read_dir(&self.dir)? {
            let sub = sub?.path();
            if !sub.is_dir() {
                continue;
            }
            for e in std::fs::read_dir(&sub)? {
                let e = e?;
                let name = e.file_name().to_string_lossy().into_owned();
                let md = match e.metadata() {
                    Ok(md) => md,
                    // gone since the folder was read: a block renamed into its place meanwhile (the threads work on
                    // while a sweep runs), a file an older sweep removed
                    Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
                    Err(e) => return Err(e),
                };
                let old = md.modified().ok().and_then(|t| SystemTime::now().duration_since(t).ok()).is_some_and(|a| a > STALE_TMP);
                if names.contains(&name) {
                    bytes += md.len();
                } else if !name.starts_with('.') || old {
                    let _ = std::fs::remove_file(e.path());
                }
            }
        }
        if stale() {
            return Ok(());
        }
        self.bytes.store(bytes, Ordering::Relaxed);
        self.check_room();
        let to_make = rest.len();
        let mut q = self.q.lock();
        q.rest = rest;
        if to_make > 0 {
            q.told = false;
            self.cv.notify_all();
        }
        drop(q);
        log::line(&format!(
            "kept blocks: {} of {} ({:.0} MB), {to_make} to compress at brotli's best{}",
            live.len() - to_make,
            live.len(),
            bytes as f64 / 1e6,
            if self.keeping() { "" } else { " (no room: not kept)" }
        ));
        Ok(())
    }

    /// whether the folder may grow: below MAX_BYTES, and MIN_FREE free on its disk
    fn check_room(&self) {
        use std::os::unix::ffi::OsStrExt;
        let free = std::ffi::CString::new(self.dir.as_os_str().as_bytes()).ok().and_then(|p| {
            // SAFETY: statvfs writes the struct it is given, and only that; the path is NUL-terminated
            let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
            (unsafe { libc::statvfs(p.as_ptr(), &mut s) } == 0).then(|| s.f_bavail as u64 * s.f_frsize as u64)
        });
        let ok = self.bytes.load(Ordering::Relaxed) < MAX_BYTES && free.is_none_or(|f| f >= MIN_FREE);
        self.room.store(ok, Ordering::Relaxed);
        *self.room_at.lock() = Instant::now();
    }

    fn work(&self) {
        loop {
            let job = {
                let mut q = self.q.lock();
                loop {
                    // no room, or the CPU quota nearly used: the queues wait as they are
                    if (!q.first.is_empty() || !q.rest.is_empty()) && (!self.keeping() || self.slice_full()) {
                        self.cv.wait_for(&mut q, PACE_EVERY.min(ROOM_EVERY));
                        continue;
                    }
                    if let Some(j) = q.first.pop_front() {
                        q.in_first.remove(&j.key);
                        break j;
                    }
                    if let Some(j) = q.rest.pop_front() {
                        break j;
                    }
                    if !q.told {
                        q.told = true;
                        if q.made > 0 {
                            log::line(&format!(
                                "kept blocks: {} compressed, {:.0} MB kept in all",
                                q.made,
                                self.bytes.load(Ordering::Relaxed) as f64 / 1e6
                            ));
                        }
                        q.made = 0;
                    }
                    self.cv.wait(&mut q);
                }
            };
            if self.path(&job.key).exists() {
                continue;
            }
            match self.make(&job) {
                Ok(true) => {
                    let mut q = self.q.lock();
                    q.made += 1;
                    if q.made.is_multiple_of(64) {
                        self.check_room();
                    }
                }
                Ok(false) => {}
                Err(e) => log::error(&format!("kept blocks: {} block {}: {e}", job.real, job.b)),
            }
        }
    }

    /// the block compressed and kept, or kept as one sent as it is (false: not, as the file changed)
    fn make(&self, job: &Job) -> io::Result<bool> {
        let f = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW).open(&job.real)?;
        let md = f.metadata()?;
        if !md.is_file() || md.size() != job.size || (md.mtime(), md.mtime_nsec()) != job.mtime {
            return Ok(false);
        }
        let start = job.b * BLOCK;
        let mut buf = vec![0u8; BLOCK.min(job.size.saturating_sub(start)) as usize];
        let got = read_full_at(&f, &mut buf, start)?;
        if got < buf.len() {
            return Ok(false);
        }
        // (no brotli for what zstd at its fastest cannot make smaller: the movies' video)
        let fast = zstd::bulk::compress(&buf, 1)?;
        let z = if (fast.len() as f64) < buf.len() as f64 * 0.995 { compress(&buf)? } else { Vec::new() };
        let body: &[u8] = if !z.is_empty() && z.len() < buf.len() { &z } else { &[] };
        self.put(&job.key, body)?;
        Ok(true)
    }

    fn put(&self, k: &Key, body: &[u8]) -> io::Result<()> {
        let to = self.path(k);
        let sub = to.parent().expect("a folder");
        std::fs::DirBuilder::new().recursive(true).mode(0o700).create(sub)?;
        let tmp = sub.join(format!(".{}.{}", hex(k), self.tmp_n.fetch_add(1, Ordering::Relaxed)));
        let r = (|| {
            let mut w = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&tmp)?;
            w.write_all(body)?;
            // on the disk before its name: after a power cut a renamed file could come back cut short, and be served
            // as a whole brotli stream that Cloudflare keeps for good
            w.sync_all()?;
            std::fs::rename(&tmp, &to)
        })();
        if r.is_err() {
            let _ = std::fs::remove_file(&tmp);
        }
        r?;
        self.bytes.fetch_add(body.len() as u64, Ordering::Relaxed);
        Ok(())
    }

    /// what the start-up lines say of it
    pub fn summary(&self) -> String {
        format!("kept at brotli's best in {}, {} threads (NFSU2_COMPRESS_THREADS)", self.dir.display(), self.threads)
    }

    #[cfg(test)]
    fn dir(&self) -> &std::path::Path {
        &self.dir
    }
}

/// a block at brotli's best
pub fn compress(raw: &[u8]) -> io::Result<Vec<u8>> {
    compress_at(raw, QUALITY)
}

fn compress_at(raw: &[u8], quality: u32) -> io::Result<Vec<u8>> {
    let mut w = brotli::CompressorWriter::new(Vec::with_capacity(raw.len() / 2), 64 << 10, quality, LGWIN);
    w.write_all(raw)?;
    w.flush()?;
    Ok(w.into_inner())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app::testkit::data;
    use std::io::Read;

    fn eventually_sync(what: &str, ok: impl Fn() -> bool) {
        let t0 = std::time::Instant::now();
        while !ok() {
            assert!(t0.elapsed() < Duration::from_secs(20), "{what}");
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    fn unbr(z: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        brotli::Decompressor::new(z, 4096).read_to_end(&mut out).unwrap();
        out
    }

    fn file_with(dir: &std::path::Path, name: &str, body: &[u8]) -> (String, std::fs::Metadata) {
        let p = dir.join(name);
        std::fs::write(&p, body).unwrap();
        (p.to_string_lossy().into_owned(), std::fs::metadata(&p).unwrap())
    }

    // a key names one block of one version: another block, size or time is another key
    #[test]
    fn keys_name_a_version_and_a_block() {
        let d = tempfile::tempdir().unwrap();
        let (real, md) = file_with(d.path(), "f", b"one");
        assert_eq!(key(&real, &md, 0), key(&real, &md, 0));
        assert_ne!(key(&real, &md, 0), key(&real, &md, 1));
        let (_, md2) = file_with(d.path(), "f", b"one more");
        assert_ne!(key(&real, &md, 0), key(&real, &md2, 0));
        assert_eq!(hex(&key(&real, &md, 0)).len(), 32);
    }

    // queued blocks are made, the brotli stream gives the bytes back, a lookup finds them; noise is never kept
    #[test]
    fn queued_blocks_are_made_and_found() {
        let d = tempfile::tempdir().unwrap();
        // blocks 0 and 1 compress, 2 is noise (kept as one sent as it is), 3 compresses (and is shorter)
        let mut body = data(2 << 20, true);
        body.extend(data(1 << 20, false));
        body.extend(data(3000, true));
        let (real, md) = file_with(d.path(), "game.bin", &body);
        let k = KeptBlocks::open(d.path().join("blocks"), 1).unwrap();
        for b in [0, 1, 2, 3] {
            assert!(matches!(k.lookup(&key(&real, &md, b)), Kept::No));
            k.queue(&real, &md, b);
        }
        eventually_sync("block 3 kept", || matches!(k.lookup(&key(&real, &md, 3)), Kept::Br(..)));
        for b in [0, 1, 3u64] {
            let Kept::Br(mut f, len) = k.lookup(&key(&real, &md, b)) else { panic!("block {b} kept") };
            let mut z = Vec::new();
            f.read_to_end(&mut z).unwrap();
            assert_eq!(z.len() as u64, len);
            let (s, e) = ((b << 20) as usize, (((b + 1) << 20) as usize).min(body.len()));
            assert_eq!(unbr(&z), &body[s..e], "block {b}");
        }
        eventually_sync("noise kept as one sent as it is", || matches!(k.lookup(&key(&real, &md, 2)), Kept::AsIs));
        // nothing half-written is left
        let names: Vec<String> = std::fs::read_dir(k.dir())
            .unwrap()
            .flat_map(|s| std::fs::read_dir(s.unwrap().path()).unwrap())
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        assert_eq!(names.len(), 4, "{names:?}");
        assert!(names.iter().all(|n| !n.starts_with('.')));
    }

    // a file changed after its block was queued: nothing kept for the old version
    #[test]
    fn a_changed_file_is_not_kept_as_the_old_version() {
        let d = tempfile::tempdir().unwrap();
        let (real, md) = file_with(d.path(), "game.bin", &data(1 << 20, true));
        let k = KeptBlocks::open(d.path().join("blocks"), 0).unwrap();
        file_with(d.path(), "game.bin", &data(1 << 19, true));
        let job = Job { real: real.clone(), size: md.size(), mtime: (md.mtime(), md.mtime_nsec()), b: 0, key: key(&real, &md, 0) };
        assert!(!k.make(&job).unwrap());
        assert!(matches!(k.lookup(&key(&real, &md, 0)), Kept::No));
    }

    // a sweep queues every block of the manifest's files and removes what is kept for no block of theirs
    #[test]
    fn a_sweep_makes_the_manifest_and_removes_the_rest() {
        let d = tempfile::tempdir().unwrap();
        let (a, _) = file_with(d.path(), "a.bin", &data((1 << 20) + 5000, true));
        let (b, _) = file_with(d.path(), "b.bin", &data(100, true));
        let k = KeptBlocks::open(d.path().join("blocks"), 2).unwrap();
        // an old version's block and a crash's half-written file
        k.put(&[7; 16], b"old").unwrap();
        let tmp = k.path(&[8; 16]).with_file_name(".x.1");
        std::fs::create_dir_all(tmp.parent().unwrap()).unwrap();
        std::fs::write(&tmp, b"half").unwrap();
        let locate = |p: &str| {
            let md = std::fs::metadata(p).ok()?;
            Some((p.to_string(), md))
        };
        k.sweep("m1", vec![a.clone(), b.clone(), "/no/such/file".into()], locate);
        let keys = [(a.as_str(), 0u64), (a.as_str(), 1), (b.as_str(), 0)].map(|(p, n)| key(p, &std::fs::metadata(p).unwrap(), n));
        eventually_sync("the manifest's blocks kept", || keys.iter().all(|x| !matches!(k.lookup(x), Kept::No)));
        assert!(!k.path(&[7; 16]).exists(), "a block of no file of the manifest is removed");
        assert!(tmp.exists(), "a file being written now is left alone");
        // the same manifest again: no sweep; a new one sweeps
        assert_eq!(k.sweeps.load(Ordering::SeqCst), 1);
        k.sweep("m1", vec![a.clone()], locate);
        assert_eq!(k.sweeps.load(Ordering::SeqCst), 1);
        k.sweep("m2", vec![a.clone()], locate);
        eventually_sync("b's block removed", || !k.path(&keys[2]).exists());
        assert!(k.path(&keys[0]).exists());
    }

    /// the CPU quota the threads read, at `share` of its 4 cores used (read at once)
    fn quota(k: &KeptBlocks, share: Arc<AtomicU64>) {
        let mut p = k.pace.lock();
        p.sample = Some(Box::new(move || {
            Some(QuotaUse { group: "/test.slice".into(), cores: 4.0, used: f64::from_bits(share.load(Ordering::SeqCst)) * 4.0 })
        }));
        p.at = Instant::now().checked_sub(PACE_EVERY).unwrap_or(p.at);
    }

    fn not_kept(k: &KeptBlocks, real: &str, md: &std::fs::Metadata) -> bool {
        matches!(k.lookup(&key(real, md, 0)), Kept::No)
    }

    // the CPU quota the server shares nearly used: no block is made until it is under RESUME_AT, the queue kept
    #[test]
    fn a_full_cpu_quota_holds_the_blocks_back() {
        let d = tempfile::tempdir().unwrap();
        let (real, md) = file_with(d.path(), "game.bin", &data(1 << 20, true));
        let k = KeptBlocks::open(d.path().join("blocks"), 1).unwrap();
        let share = Arc::new(AtomicU64::new(0.9f64.to_bits()));
        quota(&k, share.clone());
        k.queue(&real, &md, 0);
        std::thread::sleep(PACE_EVERY * 6);
        assert!(not_kept(&k, &real, &md), "made at 90 %");
        // under PAUSE_AT, not under RESUME_AT: still waiting
        share.store(0.6f64.to_bits(), Ordering::SeqCst);
        std::thread::sleep(PACE_EVERY * 6);
        assert!(not_kept(&k, &real, &md), "made at 60 % after 90 %");
        share.store(0.3f64.to_bits(), Ordering::SeqCst);
        eventually_sync("made at 30 %", || !not_kept(&k, &real, &md));
    }

    // no room (the folder or the disk full): what is queued waits, and is made once there is room again, which is
    // looked at again by itself; the answers keep blocks again too.  (The threads dropped every job without room,
    // and only a block made looked at the room again: none ever was.)
    #[test]
    fn blocks_wait_for_room_and_are_made_once_there_is() {
        let d = tempfile::tempdir().unwrap();
        let (real, md) = file_with(d.path(), "game.bin", &data(1 << 20, true));
        let k = KeptBlocks::open(d.path().join("blocks"), 1).unwrap();
        // held back by the quota while the room goes
        let share = Arc::new(AtomicU64::new(0.9f64.to_bits()));
        quota(&k, share.clone());
        k.queue(&real, &md, 0);
        k.bytes.store(MAX_BYTES, Ordering::Relaxed);
        k.check_room();
        assert!(!k.keeping());
        share.store(0f64.to_bits(), Ordering::SeqCst);
        std::thread::sleep(ROOM_EVERY * 6);
        assert!(not_kept(&k, &real, &md), "made without room");
        k.bytes.store(0, Ordering::Relaxed);
        eventually_sync("made once there is room", || !not_kept(&k, &real, &md));
        assert!(k.keeping());
    }

    // a sweep that fails (a folder it cannot read) is not taken as done: the same manifest is swept again
    #[test]
    fn a_failed_sweep_is_swept_again() {
        // SAFETY: geteuid has no preconditions
        if unsafe { libc::geteuid() } == 0 {
            return; // root reads any folder
        }
        use std::os::unix::fs::PermissionsExt;
        let d = tempfile::tempdir().unwrap();
        let (a, _) = file_with(d.path(), "a.bin", &data(100, true));
        let k = KeptBlocks::open(d.path().join("blocks"), 0).unwrap();
        let locked = k.dir().join("zz");
        std::fs::create_dir(&locked).unwrap();
        std::fs::set_permissions(&locked, std::fs::Permissions::from_mode(0o000)).unwrap();
        let locate = |p: &str| Some((p.to_string(), std::fs::metadata(p).ok()?));
        k.sweep("m1", vec![a.clone()], locate);
        eventually_sync("the failed sweep forgotten", || k.swept.lock().is_empty());
        std::fs::set_permissions(&locked, std::fs::Permissions::from_mode(0o700)).unwrap();
        k.sweep("m1", vec![a.clone()], locate);
        assert_eq!(k.sweeps.load(Ordering::SeqCst), 2);
        eventually_sync("a's block queued", || k.q.lock().rest.len() == 1);
        assert_eq!(*k.swept.lock(), "m1");
    }

    /// a measurement, not a check: blocks of $NFSU2_BENCH_FILE at brotli's best, the time and size each
    /// (cargo test --release -- --ignored bench_best --nocapture)
    #[test]
    #[ignore]
    fn bench_best() {
        let Ok(path) = std::env::var("NFSU2_BENCH_FILE") else { return };
        let raw = std::fs::read(&path).unwrap();
        let (mut n, mut z, mut t) = (0usize, 0usize, 0f64);
        for c in raw.chunks(BLOCK as usize).take(16) {
            let t0 = std::time::Instant::now();
            z += compress_at(c, 11).unwrap().len();
            t += t0.elapsed().as_secs_f64();
            n += c.len();
        }
        println!(
            "{:.1} MB -> {:.1} MB ({:.1} %) in {:.1} s, {:.2} MB/s",
            n as f64 / 1e6,
            z as f64 / 1e6,
            100.0 * z as f64 / n as f64,
            t,
            n as f64 / 1e6 / t
        );
    }
}
