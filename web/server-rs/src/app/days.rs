//! How many people come each day (UTC), each browser once a day, by the random id its page keeps (web/src/app/page-id.ts):
//! the ones that opened the page, played (pressed Play, or came onto the switch), drove in free roam, and were in
//! the game's Online mode.  The page says "page" at a load and "play" at Play (/seen); the switch's hellos and
//! [`Days::sweep`] (every `SWEEP_EVERY`: who is in free roam or Online now, `LanSwitch::who`) add the rest.  Each
//! kind counts in the ones before it too (a browser seen in free roam or Online played, whatever /seen said).
//!
//! Only a keyed hash of an id is kept (64 bits of HMAC-SHA256), with a new random key each day, and the day's sets go
//! at its end: nothing links one day's browsers to the next day's, or to a ban.  The day so far is in the state
//! folder's `days.json` (every `SWEEP_EVERY` when it changed, and at the stop), so a restart counts on; each day that
//! ended is a line of `days.csv` there, and of the console (`[days]`).  A browser on two devices is two, a private
//! window one a load, and a script can make up ids: the counts are for the operator, not for anything that matters.
//! A day's sets hold `MAX_A_DAY` at most.

use std::collections::HashSet;
use std::io::Write;
use std::path::PathBuf;

use hmac::{Hmac, Mac};
use parking_lot::Mutex;
use serde_json::Value;

use nfsu2_util::{js, log};

/// what a browser did (each counts in the ones before it too)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Seen {
    Page = 0,
    Play = 1,
    Roam = 2,
    Online = 3,
}

/// days.json's names of the sets, in `Seen`'s order
const KINDS: [&str; 4] = ["page", "play", "roam", "online"];
pub const CSV_HEAD: &str = "day,opened the page,played,free roam,Online";
/// a day's set holds this many at most (8 bytes each, in memory and in days.json)
pub const MAX_A_DAY: usize = 1_000_000;

/// "2026-10-30", the UTC day of `ms`
pub fn day_of(ms: i64) -> String {
    let (y, m, d, ..) = js::utc_parts(ms);
    format!("{y:04}-{m:02}-{d:02}")
}

/// a page id as the page makes them (22 characters of base64url; 64 at most, as the hello's)
pub fn id_ok(id: &str) -> bool {
    (1..=64).contains(&id.len()) && id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

struct Today {
    day: String,
    key: Vec<u8>,
    sets: [HashSet<u64>; 4],
    /// since days.json was written
    changed: bool,
}

impl Today {
    fn new(day: String) -> Today {
        Today { day, key: nfsu2_util::rand::bytes(32), sets: Default::default(), changed: true }
    }

    fn counts(&self) -> [usize; 4] {
        [0, 1, 2, 3].map(|i| self.sets[i].len())
    }

    fn hash(&self, id: &str) -> u64 {
        let mut mac = <Hmac<sha2::Sha256> as Mac>::new_from_slice(&self.key).expect("HMAC takes any key");
        mac.update(id.as_bytes());
        u64::from_le_bytes(mac.finalize().into_bytes()[..8].try_into().expect("8 bytes"))
    }

    fn to_json(&self) -> String {
        let mut m = serde_json::Map::new();
        m.insert("day".into(), self.day.clone().into());
        m.insert("key".into(), js::base64url(&self.key).into());
        for (i, k) in KINDS.iter().enumerate() {
            let b: Vec<u8> = self.sets[i].iter().flat_map(|h| h.to_le_bytes()).collect();
            m.insert((*k).into(), js::base64url(&b).into());
        }
        Value::Object(m).to_string()
    }

    fn from_json(s: &str) -> Option<Today> {
        let v: Value = serde_json::from_str(s).ok()?;
        let day = v.get("day")?.as_str()?.to_string();
        let key = js::from_base64url(v.get("key")?.as_str()?);
        if key.len() != 32 {
            return None;
        }
        let mut t = Today { day, key, sets: Default::default(), changed: false };
        for (i, k) in KINDS.iter().enumerate() {
            let b = js::from_base64url(v.get(*k).and_then(Value::as_str).unwrap_or(""));
            t.sets[i] = b.chunks_exact(8).take(MAX_A_DAY).map(|c| u64::from_le_bytes(c.try_into().expect("8 bytes"))).collect();
        }
        Some(t)
    }
}

/// "4210 opened the page, 3380 played, 2950 in free roam, 610 in Online"
pub fn counts_text(c: [usize; 4]) -> String {
    format!("{} opened the page, {} played, {} in free roam, {} in Online", c[0], c[1], c[2], c[3])
}

pub struct Days {
    /// days.json and days.csv
    file: PathBuf,
    csv: PathBuf,
    t: Mutex<Today>,
}

impl Days {
    /// The day so far from the state folder's days.json (a day that ended meanwhile goes to days.csv first), or a new
    /// one.
    pub fn open(state: &str, now: i64) -> Days {
        let dir = PathBuf::from(state);
        let (file, csv) = (dir.join("days.json"), dir.join("days.csv"));
        let had = std::fs::read_to_string(&file).ok().and_then(|s| Today::from_json(&s));
        let d = Days { file, csv, t: Mutex::new(had.unwrap_or_else(|| Today::new(day_of(now)))) };
        d.roll(&mut d.t.lock(), now);
        d
    }

    /// A browser seen: `id` as its page keeps it (`id_ok`), `what` it did.
    pub fn seen(&self, id: &str, what: Seen, now: i64) {
        if !id_ok(id) {
            return;
        }
        let mut t = self.t.lock();
        self.roll(&mut t, now);
        let h = t.hash(id);
        for i in 0..=what as usize {
            if t.sets[i].len() < MAX_A_DAY && t.sets[i].insert(h) {
                t.changed = true;
            }
        }
    }

    /// the day so far: (its date, `Seen`'s counts)
    pub fn today(&self, now: i64) -> (String, [usize; 4]) {
        let mut t = self.t.lock();
        self.roll(&mut t, now);
        (t.day.clone(), t.counts())
    }

    /// The days that ended (days.csv's lines without its head), the last `n` of them
    pub fn history(&self, n: usize) -> Vec<String> {
        let s = std::fs::read_to_string(&self.csv).unwrap_or_default();
        let lines: Vec<String> = s.lines().filter(|l| !l.is_empty() && *l != CSV_HEAD).map(String::from).collect();
        lines[lines.len().saturating_sub(n)..].to_vec()
    }

    /// days.json written when the day changed since; true when it was
    pub fn save(&self) -> Result<bool, String> {
        let body = {
            let mut t = self.t.lock();
            if !t.changed {
                return Ok(false);
            }
            t.changed = false;
            t.to_json()
        };
        nfsu2_claims::write_atomic(&self.file, body.as_bytes()).map(|_| true)
    }

    /// a day that ended: its line in days.csv (once: a line of that day already there stays the only one) and the
    /// console, then a new day with a new key
    fn roll(&self, t: &mut Today, now: i64) {
        let day = day_of(now);
        if t.day == day {
            return;
        }
        let c = t.counts();
        log::line(&format!("[days] {} (UTC): {}", t.day, counts_text(c)));
        let had = std::fs::read_to_string(&self.csv).unwrap_or_default();
        if !had.lines().any(|l| l.starts_with(&format!("{},", t.day))) {
            let head = if had.is_empty() { format!("{CSV_HEAD}\n") } else { String::new() };
            let line = format!("{head}{},{},{},{},{}\n", t.day, c[0], c[1], c[2], c[3]);
            let w = std::fs::OpenOptions::new().create(true).append(true).open(&self.csv).and_then(|mut f| f.write_all(line.as_bytes()));
            if let Err(e) = w {
                log::error(&format!("{}: {e}", self.csv.display()));
            }
        }
        *t = Today::new(day);
    }

    /// for the tests: the day's state as days.json has it
    #[cfg(test)]
    fn json(&self) -> Value {
        serde_json::from_str(&self.t.lock().to_json()).unwrap_or(Value::Null)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DAY: i64 = 86_400_000;
    /// 2026-10-30T12:00Z
    const T0: i64 = 1_793_361_600_000;

    fn dir(name: &str) -> String {
        let d = std::env::temp_dir().join(format!("nfsu2-days-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d.display().to_string()
    }

    #[test]
    fn counts_each_browser_once_and_in_the_kinds_before() {
        let d = Days::open(&dir("once"), T0);
        let a = "AAAAAAAAAAAAAAAAAAAAAA";
        d.seen(a, Seen::Page, T0);
        d.seen(a, Seen::Page, T0 + 1);
        d.seen(a, Seen::Play, T0 + 2);
        d.seen("BBBBBBBBBBBBBBBBBBBBBB", Seen::Online, T0 + 3);
        d.seen("CCCCCCCCCCCCCCCCCCCCCC", Seen::Roam, T0 + 4);
        d.seen("bad id!", Seen::Online, T0 + 5);
        d.seen("", Seen::Page, T0 + 5);
        assert_eq!(d.today(T0 + 6), ("2026-10-30".into(), [3, 3, 2, 1]));
    }

    #[test]
    fn a_restart_counts_on_and_a_new_day_goes_to_the_csv() {
        let s = dir("restart");
        let d = Days::open(&s, T0);
        d.seen("AAAAAAAAAAAAAAAAAAAAAA", Seen::Play, T0);
        assert!(d.save().unwrap());
        assert!(!d.save().unwrap(), "nothing new: not written again");
        let key = d.json()["key"].clone();
        drop(d);

        let d = Days::open(&s, T0 + 1000);
        assert_eq!(d.json()["key"], key, "the day's key kept");
        d.seen("AAAAAAAAAAAAAAAAAAAAAA", Seen::Play, T0 + 1000);
        d.seen("BBBBBBBBBBBBBBBBBBBBBB", Seen::Page, T0 + 1000);
        assert_eq!(d.today(T0 + 1000).1, [2, 1, 0, 0]);
        d.save().unwrap();
        drop(d);

        // started again the next day: the day before is a line, the new day starts at 0 with another key
        let d = Days::open(&s, T0 + DAY);
        assert_eq!(d.today(T0 + DAY), ("2026-10-31".into(), [0, 0, 0, 0]));
        assert_ne!(d.json()["key"], key);
        assert_eq!(d.history(30), vec!["2026-10-30,2,1,0,0".to_string()]);
        let csv = std::fs::read_to_string(PathBuf::from(&s).join("days.csv")).unwrap();
        assert_eq!(csv, format!("{CSV_HEAD}\n2026-10-30,2,1,0,0\n"));

        // a day rolls over while running too; a day's line is never written twice
        d.seen("AAAAAAAAAAAAAAAAAAAAAA", Seen::Online, T0 + DAY);
        assert_eq!(d.today(T0 + 2 * DAY).1, [0, 0, 0, 0]);
        assert_eq!(d.history(30), vec!["2026-10-30,2,1,0,0".to_string(), "2026-10-31,1,1,1,1".to_string()]);
        let _ = std::fs::remove_dir_all(&s);
    }

    #[test]
    fn no_id_is_kept() {
        let s = dir("noid");
        let d = Days::open(&s, T0);
        d.seen("AAAAAAAAAAAAAAAAAAAAAA", Seen::Play, T0);
        d.save().unwrap();
        let f = std::fs::read_to_string(PathBuf::from(&s).join("days.json")).unwrap();
        assert!(!f.contains("AAAAAAAAAAAAAAAAAAAAAA"));
        let _ = std::fs::remove_dir_all(&s);
    }

    #[test]
    fn days_are_utc() {
        assert_eq!(day_of(T0), "2026-10-30");
        assert_eq!(day_of(T0 + DAY / 2 - 1), "2026-10-30");
        assert_eq!(day_of(T0 + DAY / 2), "2026-10-31");
    }
}
