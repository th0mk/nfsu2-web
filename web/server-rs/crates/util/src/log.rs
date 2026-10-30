//! The console: one line at a time to stdout (or stderr), and [`limited`]: the lines a client causes (refused
//! requests and upgrades, joins and leaves, the access log, Online lines), at most `per` a minute of one topic for
//! one client, then one line that says how many were left out.  So one client can no longer fill the journal, push
//! the VPS's other logs out or make journald drop real players' lines (the audit's DEP-4).  The client is only the
//! key of the count: no line names its address.

use std::io::Write;
use std::sync::OnceLock;
use std::time::Duration;

use std::collections::{BTreeMap, HashMap};

use parking_lot::Mutex;

use crate::clock::now_ms;

/// one line to stdout (console.log)
pub fn line(s: &str) {
    write_line(&mut console(), s);
}

fn write_line(o: &mut impl Write, s: &str) {
    let _ = o.write_all(s.as_bytes());
    let _ = o.write_all(b"\n");
    let _ = o.flush();
    keep_line(s);
}

/// The console kept in files of its own as well, past a restart and a new container (`keep`): console.log in a
/// folder, each line after its time (UTC), and when it would grow past KEEP_BYTES it becomes console.1.log (that one
/// console.2.log, ...), at most KEEP_FILES in all.  Docker's own log of a container goes with the container, at every
/// release (LESSONS L-178).
pub const KEEP_BYTES: u64 = 32 << 20;
pub const KEEP_FILES: usize = 8;

struct Kept {
    dir: std::path::PathBuf,
    file: std::fs::File,
    size: u64,
    max: u64,
}

fn kept() -> &'static OnceLock<Mutex<Kept>> {
    static K: OnceLock<Mutex<Kept>> = OnceLock::new();
    &K
}

/// the name of the n-th kept file (0: the one written to)
pub fn kept_name(n: usize) -> String {
    if n == 0 { "console.log".into() } else { format!("console.{n}.log") }
}

/// From now on every console line (and every error line) also goes into `dir`/console.log (the folder private to this
/// user, the files 0600), after what an earlier start left there.  Once a process; Err: the folder or the file cannot
/// be made.
pub fn keep(dir: &std::path::Path) -> std::io::Result<()> {
    keep_sized(dir, KEEP_BYTES)
}

fn keep_sized(dir: &std::path::Path, max: u64) -> std::io::Result<()> {
    use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
    std::fs::DirBuilder::new().recursive(true).mode(0o700).create(dir)?;
    let file = std::fs::OpenOptions::new().create(true).append(true).mode(0o600).open(dir.join(kept_name(0)))?;
    // (one an earlier start left, or made otherwise: private too)
    use std::os::unix::fs::PermissionsExt;
    let _ = file.set_permissions(std::fs::Permissions::from_mode(0o600));
    let size = file.metadata()?.len();
    let _ = kept().set(Mutex::new(Kept { dir: dir.to_path_buf(), file, size, max }));
    Ok(())
}

/// a line into the kept files, when they are on (a line that cannot be written there is lost there only)
fn keep_line(s: &str) {
    let Some(k) = kept().get() else { return };
    let line = format!("{} {s}\n", crate::js::iso_string(now_ms()));
    let mut k = k.lock();
    if k.size > 0 && k.size + line.len() as u64 > k.max {
        // the oldest goes, each other moves up one, and a new console.log begins
        let _ = std::fs::remove_file(k.dir.join(kept_name(KEEP_FILES - 1)));
        for n in (0..KEEP_FILES - 1).rev() {
            let _ = std::fs::rename(k.dir.join(kept_name(n)), k.dir.join(kept_name(n + 1)));
        }
        use std::os::unix::fs::OpenOptionsExt;
        match std::fs::OpenOptions::new().create(true).append(true).mode(0o600).open(k.dir.join(kept_name(0))) {
            Ok(f) => {
                k.file = f;
                k.size = 0;
            }
            Err(_) => return,
        }
    }
    if k.file.write_all(line.as_bytes()).is_ok() {
        k.size += line.len() as u64;
    }
}

/// stdout, locked: one writer at a time (in this crate's own tests, a buffer they read back)
#[cfg(not(test))]
fn console() -> std::io::StdoutLock<'static> {
    std::io::stdout().lock()
}
#[cfg(test)]
use tests::console;

/// one line to stderr (console.error)
pub fn error(s: &str) {
    let out = std::io::stderr();
    let mut o = out.lock();
    let _ = o.write_all(s.as_bytes());
    let _ = o.write_all(b"\n");
    let _ = o.flush();
    keep_line(s);
}

struct Quiet {
    t: i64,
    n: u32,
    left: u32,
    kind: String,
    topic: String,
}

/// The counts by topic and client, in the order they began (the lines that say how many were left out come in that
/// order).  A count leaves and comes back at the end each minute, among up to 10,000 of them: a place
/// number per count keeps the order with no shift of the others (an IndexMap's ordered removal moved them all, under
/// the lock every client's line takes).
#[derive(Default)]
struct Counts {
    place: HashMap<String, u64>,
    order: BTreeMap<u64, (String, Quiet)>,
    next: u64,
}

impl Counts {
    fn get(&self, k: &str) -> Option<&Quiet> {
        self.place.get(k).and_then(|p| self.order.get(p)).map(|(_, q)| q)
    }
    fn get_mut(&mut self, k: &str) -> Option<&mut Quiet> {
        let p = *self.place.get(k)?;
        self.order.get_mut(&p).map(|(_, q)| q)
    }
    fn contains_key(&self, k: &str) -> bool {
        self.place.contains_key(k)
    }
    fn len(&self) -> usize {
        self.place.len()
    }
    /// at the end of the order
    fn insert(&mut self, k: String, q: Quiet) {
        let p = self.next;
        self.next += 1;
        if let Some(old) = self.place.insert(k.clone(), p) {
            self.order.remove(&old);
        }
        self.order.insert(p, (k, q));
    }
    fn remove(&mut self, k: &str) -> Option<Quiet> {
        let p = self.place.remove(k)?;
        self.order.remove(&p).map(|(_, q)| q)
    }
    /// every count, in order
    fn drain(&mut self) -> Vec<Quiet> {
        self.place.clear();
        std::mem::take(&mut self.order).into_values().map(|(_, q)| q).collect()
    }
    /// the counts `keep` says no to go, in order, each seen by `keep`
    fn retain(&mut self, mut keep: impl FnMut(&Quiet) -> bool) {
        let gone: Vec<u64> = self.order.iter().filter(|(_, (_, q))| !keep(q)).map(|(p, _)| *p).collect();
        for p in gone {
            if let Some((k, _)) = self.order.remove(&p) {
                self.place.remove(&k);
            }
        }
    }
}

fn quiet() -> &'static Mutex<Counts> {
    static Q: OnceLock<Mutex<Counts>> = OnceLock::new();
    Q.get_or_init(|| {
        // every 15 s: the counts of the minutes that are over
        std::thread::Builder::new()
            .name("log-quiet".into())
            .spawn(|| {
                loop {
                    std::thread::sleep(Duration::from_secs(15));
                    flush_all(false);
                }
            })
            .ok();
        Mutex::new(Counts::default())
    })
}

/// the line that says how many of a minute's lines were left out, if any were
fn left_out(b: &Quiet) -> Option<String> {
    (b.left > 0).then(|| format!("[{}] {} more {} from one client in that minute (not shown)", b.kind, b.left, b.topic))
}

fn flush_all(force: bool) {
    let now = now_ms();
    let mut out = Vec::new();
    let mut q = quiet().lock();
    q.retain(|b| {
        let over = force || now - b.t >= 60_000;
        if over {
            out.extend(left_out(b));
        }
        !over
    });
    if out.is_empty() {
        return;
    }
    // (the console taken before the counts are let go: see `limited`)
    let mut c = console();
    drop(q);
    for l in out {
        write_line(&mut c, &l);
    }
}

/// A line client `key` caused: shown while it is among the first `per` of its `kind` and `topic` in a minute, else
/// counted, and the count of those left out said once the minute is over ("[kind] N more topic from one client in
/// that minute (not shown)").  What goes out is decided under the counts' lock and written after it (with
/// --access-log every request comes here: none waits on another's write to the console).  A call that ends a minute
/// with lines left out takes the console before it lets the counts go, so that its "N more" line comes before every
/// line decided after it (a client over its share: once a minute at most).
pub fn limited(kind: &str, topic: &str, key: &str, text: &str, per: u32) {
    let now = now_ms();
    let k = format!("{kind} {topic} {key}");
    let mut out: Vec<String> = Vec::new();
    let (show, held) = {
        let mut q = quiet().lock();
        if q.get(&k).is_some_and(|b| now - b.t >= 60_000)
            && let Some(b) = q.remove(&k)
        {
            out.extend(left_out(&b));
        }
        if !q.contains_key(&k) {
            if q.len() >= 10_000 {
                // every count goes out at once: at most 10,000 are kept
                out.extend(q.drain().iter().filter_map(left_out));
            }
            q.insert(k.clone(), Quiet { t: now, n: 0, left: 0, kind: kind.to_string(), topic: topic.to_string() });
        }
        let b = q.get_mut(&k).expect("just made");
        b.n += 1;
        if b.n > per {
            b.left += 1;
        }
        (b.n <= per, (!out.is_empty()).then(console))
    };
    match held {
        Some(mut c) => {
            for l in &out {
                write_line(&mut c, l);
            }
            if show {
                write_line(&mut c, text);
            }
        }
        None if show => line(text),
        None => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use parking_lot::{ReentrantMutex, ReentrantMutexGuard};
    use std::cell::RefCell;
    use std::time::Instant;

    /// the console of this crate's tests: what was written, for them to read back (reentrant, as stdout's lock is)
    static SINK: ReentrantMutex<RefCell<Vec<u8>>> = parking_lot::const_reentrant_mutex(RefCell::new(Vec::new()));

    pub(super) struct Console(ReentrantMutexGuard<'static, RefCell<Vec<u8>>>);

    impl Write for Console {
        fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
            self.0.borrow_mut().extend_from_slice(b);
            Ok(b.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }

    pub(super) fn console() -> Console {
        Console(SINK.lock())
    }

    fn written(prefix: &str) -> Vec<String> {
        let g = SINK.lock();
        let all = String::from_utf8_lossy(&g.borrow()).into_owned();
        all.lines().filter(|l| l.starts_with(prefix)).map(str::to_string).collect()
    }

    // The kept files: every console and error line after its time, after what was there before; past their size the
    // file turns over, at most KEEP_FILES of them, the newest lines always in console.log.  (The only test that turns
    // them on: once a process.)
    #[test]
    fn the_kept_files_take_every_line_and_turn_over() {
        use std::os::unix::fs::PermissionsExt;
        let d = tempfile::tempdir().unwrap();
        let dir = d.path().join("log");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("console.log"), "2026-01-01T00:00:00.000Z from the start before\n").unwrap();
        keep_sized(&dir, 400).unwrap();
        line("[kept] one");
        error("[kept] an error");
        let s = std::fs::read_to_string(dir.join("console.log")).unwrap();
        let mine: Vec<&str> = s.lines().filter(|l| l.contains("[kept]") || l.contains("before")).collect();
        assert_eq!(mine[0], "2026-01-01T00:00:00.000Z from the start before", "{s}");
        for (l, want) in mine[1..].iter().zip(["[kept] one", "[kept] an error"]) {
            let (t, rest) = l.split_once(' ').unwrap();
            assert_eq!(rest, want);
            assert!(t.len() == 24 && t.ends_with('Z') && t.as_bytes()[10] == b'T', "{l}");
        }
        assert_eq!(std::fs::metadata(dir.join("console.log")).unwrap().permissions().mode() & 0o777, 0o600);
        for i in 0..200 {
            line(&format!("[kept] line {i:03} {}", "x".repeat(40)));
        }
        let names: Vec<String> = (0..KEEP_FILES + 2).map(kept_name).filter(|n| dir.join(n).exists()).collect();
        assert_eq!(names, (0..KEEP_FILES).map(kept_name).collect::<Vec<_>>(), "turned over, the oldest gone");
        let newest = std::fs::read_to_string(dir.join("console.log")).unwrap();
        assert!(newest.contains("[kept] line 199") && newest.len() <= 400, "{newest}");
        for n in 0..KEEP_FILES {
            assert!(std::fs::metadata(dir.join(kept_name(n))).unwrap().len() <= 400);
        }
        assert_eq!(std::fs::metadata(dir.join("console.log")).unwrap().permissions().mode() & 0o777, 0o600);
    }

    // the counts keep the order they began in, a count that comes back goes to the end
    #[test]
    fn counts_keep_the_maps_order() {
        let q = |t: i64| Quiet { t, n: 0, left: 0, kind: String::new(), topic: String::new() };
        let mut c = Counts::default();
        for (k, t) in [("a", 1), ("b", 2), ("c", 3)] {
            c.insert(k.into(), q(t));
        }
        assert_eq!(c.remove("a").map(|x| x.t), Some(1));
        c.insert("a".into(), q(4));
        c.retain(|x| x.t != 2);
        assert_eq!(c.len(), 2);
        assert!(c.get("b").is_none() && c.contains_key("c"));
        c.get_mut("c").unwrap().n = 7;
        assert_eq!(c.drain().iter().map(|x| (x.t, x.n)).collect::<Vec<_>>(), [(3, 7), (4, 0)]);
        assert_eq!(c.len(), 0);
    }

    // The line that says how many of a client's lines a minute left out comes before any line decided after it.  The
    // line is decided under the counts' lock and written after it, so another thread's line decided later could be
    // written first, while the console was busy: "[order] third" before "1 more".
    #[test]
    fn a_minutes_count_comes_before_the_lines_decided_after_it() {
        let (kind, topic, key) = ("order", "test lines", "client a");
        let k = format!("{kind} {topic} {key}");
        limited(kind, topic, key, "[order] first", 1);
        limited(kind, topic, key, "[order] left out", 1);
        // that minute is over
        quiet().lock().get_mut(&k).expect("its count").t -= 60_000;
        // the console busy (another line being written) while a call ends the minute: "1 more", then its own line
        let busy = console();
        let a = std::thread::spawn(move || limited(kind, topic, key, "[order] second", 2));
        // a call decided after that one: made here once that one has let the counts go (its line can be written at
        // once: this thread holds the console); not made while that one holds the counts until it has the console
        let t0 = Instant::now();
        let mut held_since: Option<Instant> = None;
        let mut third = false;
        while !third {
            assert!(t0.elapsed() < Duration::from_secs(5), "the other call did not come");
            match quiet().try_lock() {
                Some(q) => {
                    held_since = None;
                    if q.get(&k).is_some_and(|b| b.n == 1) {
                        drop(q);
                        limited(kind, topic, key, "[order] third", 2);
                        third = true;
                    }
                }
                None if held_since.is_some_and(|t| t.elapsed() > Duration::from_millis(100)) => break,
                None => held_since = held_since.or(Some(Instant::now())),
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        drop(busy);
        a.join().unwrap();
        if !third {
            limited(kind, topic, key, "[order] third", 2);
        }
        assert_eq!(
            written("[order]"),
            ["[order] first", "[order] 1 more test lines from one client in that minute (not shown)", "[order] second", "[order] third"]
        );
    }
}
