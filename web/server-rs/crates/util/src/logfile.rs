//! The log files ([`to_files`]): every console line (`log::line`, `log::error`, the lines `log::limited` shows) also
//! goes into the state folder's `logs/<day>.log` (UTC), each after its time ("2026-10-31T10:20:40.155Z "), so the log
//! outlasts a deploy and Docker's own (its `local` driver keeps 100 MB of the console: about 16 hours with 4,000
//! players, 2026-10-31).  A day that ended is gzipped (`<day>.log.gz`, a fifth of it) and kept until DAYS_KEPT days
//! are there; then it goes.  `web/deploy/nfsu2 logs [days]` sends them out.

use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use flate2::Compression;
use flate2::write::GzEncoder;
use parking_lot::Mutex;

use crate::clock::now_ms;
use crate::js::iso_string;

/// the days kept, today's included (a day's file gzipped: 20-30 MB with 4,000 players)
pub const DAYS_KEPT: i64 = 30;
const DAY_MS: i64 = 86_400_000;

struct Files {
    dir: PathBuf,
    day: i64,
    file: Option<File>,
    /// a write or an open failed, and that was said (once, until one works again)
    failed: bool,
}

static FILES: OnceLock<Mutex<Files>> = OnceLock::new();

/// one tidy at a time (the start's and a day's end's)
static TIDY: Mutex<()> = Mutex::new(());

/// "2026-10-31": a day (days since 1970) as its file is named
fn day_name(day: i64) -> String {
    iso_string(day * DAY_MS)[..10].to_string()
}

fn open(dir: &Path, day: i64) -> io::Result<File> {
    OpenOptions::new().create(true).append(true).mode(0o600).open(dir.join(format!("{}.log", day_name(day))))
}

/// From now on the console's lines go into `dir` too (made, private to this server's user, if it is not there); the
/// days that ended are gzipped and the ones past DAYS_KEPT go, in a thread of its own.  Err: `dir` or today's file
/// cannot be made, or the files were set already.
pub fn to_files(dir: &Path) -> io::Result<()> {
    std::fs::DirBuilder::new().recursive(true).mode(0o700).create(dir)?;
    let day = now_ms().div_euclid(DAY_MS);
    let file = open(dir, day)?;
    FILES
        .set(Mutex::new(Files { dir: dir.to_path_buf(), day, file: Some(file), failed: false }))
        .map_err(|_| io::Error::other("the log files were set already"))?;
    tidy(dir.to_path_buf(), day);
    Ok(())
}

/// a console line (or lines: one after another, each after the time) into today's file; at a day's end into the
/// next day's, and the day that ended gzipped
pub(crate) fn write(text: &str) {
    let Some(files) = FILES.get() else { return };
    let now = now_ms();
    let mut b = String::with_capacity(text.len() + 32);
    let t = iso_string(now);
    for l in text.split('\n') {
        b.push_str(&t);
        b.push(' ');
        b.push_str(l);
        b.push('\n');
    }
    let say = {
        let mut f = files.lock();
        let day = now.div_euclid(DAY_MS);
        let mut err = None;
        if day != f.day || f.file.is_none() {
            let was = f.day;
            f.day = day;
            match open(&f.dir, day) {
                Ok(x) => f.file = Some(x),
                Err(e) => {
                    f.file = None;
                    err = Some(e);
                }
            }
            if day != was {
                tidy(f.dir.clone(), day);
            }
        }
        if let Some(file) = f.file.as_mut()
            && let Err(e) = file.write_all(b.as_bytes())
        {
            err = Some(e);
        }
        match err {
            Some(e) if !f.failed => {
                f.failed = true;
                Some(format!("{}/{}.log: {e} (the log files stop until a line can be written again)", f.dir.display(), day_name(day)))
            }
            Some(_) => None,
            None => {
                let back = f.failed;
                f.failed = false;
                back.then(|| format!("{}/{}.log: written again", f.dir.display(), day_name(day)))
            }
        }
    };
    // (to stderr itself, after the files' lock: `log::error` would come back here)
    if let Some(s) = say {
        let mut o = io::stderr().lock();
        let _ = o.write_all(s.as_bytes());
        let _ = o.write_all(b"\n");
    }
}

/// In a thread: each day's file before `today` gzipped (`<day>.log` into `<day>.log.gz`, through a `.part` file, so a
/// stop on the way leaves the day's file as it was), and the days before the last DAYS_KEPT gone
fn tidy(dir: PathBuf, today: i64) {
    let shown = dir.display().to_string();
    let spawned = std::thread::Builder::new().name("log-tidy".into()).spawn(move || {
        let _one = TIDY.lock();
        if let Err(e) = tidy_now(&dir, today) {
            let _ = writeln!(io::stderr().lock(), "{}: {e} (the log files' tidy)", dir.display());
        }
    });
    if let Err(e) = spawned {
        let _ = writeln!(io::stderr().lock(), "{shown}: {e} (the log files' tidy)");
    }
}

/// a log file's day, "2026-10-31", and whether it is gzipped: `<day>.log` or `<day>.log.gz`
fn file_day(name: &str) -> Option<(&str, bool)> {
    let (day, rest) = name.split_at_checked(10)?;
    let b = day.as_bytes();
    let digits = b.iter().enumerate().all(|(i, c)| if i == 4 || i == 7 { *c == b'-' } else { c.is_ascii_digit() });
    match rest {
        ".log" if digits => Some((day, false)),
        ".log.gz" if digits => Some((day, true)),
        _ => None,
    }
}

fn tidy_now(dir: &Path, today: i64) -> io::Result<()> {
    let (today, oldest) = (day_name(today), day_name(today - DAYS_KEPT + 1));
    let mut names: Vec<String> = std::fs::read_dir(dir)?.filter_map(|e| e.ok()?.file_name().into_string().ok()).collect();
    names.sort();
    for name in &names {
        let path = dir.join(name);
        if name.ends_with(".part") {
            let _ = std::fs::remove_file(&path);
            continue;
        }
        let Some((day, gz)) = file_day(name) else { continue };
        if day < oldest.as_str() {
            std::fs::remove_file(&path)?;
        } else if !gz && day < today.as_str() {
            gzip(dir, day)?;
        }
    }
    Ok(())
}

/// `<day>.log` gzipped onto the end of `<day>.log.gz` (a gzip file of two members, when there was one already: a
/// day's file made again after it was gzipped), then gone
fn gzip(dir: &Path, day: &str) -> io::Result<()> {
    let (log, gz, part) = (dir.join(format!("{day}.log")), dir.join(format!("{day}.log.gz")), dir.join(format!("{day}.log.gz.part")));
    let mut out = OpenOptions::new().create(true).write(true).truncate(true).mode(0o600).open(&part)?;
    match File::open(&gz) {
        Ok(mut was) => {
            io::copy(&mut was, &mut out)?;
        }
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(e),
    }
    let mut enc = GzEncoder::new(out, Compression::default());
    io::copy(&mut File::open(&log)?, &mut enc)?;
    enc.finish()?.sync_all()?;
    std::fs::rename(&part, &gz)?;
    std::fs::remove_file(&log)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("nfsu2-logfile-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn gunzip(p: &Path) -> String {
        let mut s = String::new();
        flate2::read::MultiGzDecoder::new(File::open(p).unwrap()).read_to_string(&mut s).unwrap();
        s
    }

    #[test]
    fn a_files_day_is_its_name() {
        assert_eq!(file_day("2026-10-31.log"), Some(("2026-10-31", false)));
        assert_eq!(file_day("2026-10-31.log.gz"), Some(("2026-10-31", true)));
        for no in ["2026-10-31.log.gz.part", "2026-10-31.txt", "2026_10_31.log", "26-10-31.log", "x", ""] {
            assert_eq!(file_day(no), None, "{no}");
        }
        assert_eq!(day_name(0), "1970-01-01");
        assert_eq!(day_name(20_392), "2025-10-31");
    }

    // the days before today gzipped (onto a day gzipped already: one file of two members), today's left as it is,
    // the ones before the last DAYS_KEPT gone, a .part of a stop on the way gone, other files left alone
    #[test]
    fn the_tidy_gzips_the_days_that_ended_and_lets_the_old_ones_go() {
        let d = temp("tidy");
        let today = 20_392; // 2025-10-31
        let w = |name: &str, s: &str| std::fs::write(d.join(name), s).unwrap();
        w("2025-10-31.log", "today\n");
        w("2025-10-30.log", "yesterday\n");
        w("2025-10-29.log", "again\n");
        let mut enc = GzEncoder::new(File::create(d.join("2025-10-29.log.gz")).unwrap(), Compression::default());
        enc.write_all(b"before\n").unwrap();
        enc.finish().unwrap();
        w("2025-10-02.log.gz", "the oldest kept");
        w("2025-10-01.log.gz", "gone");
        w("2025-09-30.log", "gone");
        w("2025-10-28.log.gz.part", "a stop on the way");
        w("notes.txt", "not ours");
        tidy_now(&d, today).unwrap();
        let mut names: Vec<String> = std::fs::read_dir(&d).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).collect();
        names.sort();
        assert_eq!(names, ["2025-10-02.log.gz", "2025-10-29.log.gz", "2025-10-30.log.gz", "2025-10-31.log", "notes.txt"]);
        assert_eq!(gunzip(&d.join("2025-10-30.log.gz")), "yesterday\n");
        assert_eq!(gunzip(&d.join("2025-10-29.log.gz")), "before\nagain\n");
        assert_eq!(std::fs::read_to_string(d.join("2025-10-31.log")).unwrap(), "today\n");
        let _ = std::fs::remove_dir_all(&d);
    }
}
