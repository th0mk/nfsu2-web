//! The online races' results per persona: the one thing the online host keeps on disk
//! (<state>/online/stats.json).  What is in it is public in the game anyway (the rankings and every player's online
//! career): a persona name, its races, wins and losses per race type, ranked and unranked, rating points, best times
//! per track, its records (top speed, longest jump ...), the month they were last seen.  Nothing about who plays a
//! persona: no account name, address, password or adapter id.
//!
//! A race counts once: the first result report (rank, RESU) from one of its players that passes the checks in
//! lobby.rs; the others' reports are only compared with it.  Rating points per race: (players - place) * 100 /
//! (players - 1), so the winner gets 100 and the last 0, ranked races only.  Every count is kept twice: overall and
//! for the current month (the month's counts start again when it changes, as the game's monthly rankings did).  A
//! persona's records come from its own entry in its own reports (`add_records`: what another player's game saw of it
//! is a guess).  Personas not seen for `prune_months` months are forgotten; with MAX_PERSONAS of them, the least
//! recently seen goes for a new one (the Online audit's ONL-7: a full store refused every new player), and a persona
//! keeps its best times on MAX_BEST tracks.  The rankings are built at most every `board_ms` (the game asks for them
//! often: building one of 100,000 records took 18 ms), and names are found by their beginning in a sorted list (EA
//! Messenger's search).
//!
//! Written atomically (a temporary file, then rename), at most every `save_ms` and when the server stops, byte for
//! byte as the Node server it replaced wrote it, but for a persona's records ("rec", only once it has one; the file
//! that server left is read as it is).  A file that does not parse is kept aside (stats.json.bad-<time>) and the
//! server starts with no results.

use std::collections::HashMap;
use std::path::PathBuf;
use std::rc::Rc;

use indexmap::IndexMap;
use nfsu2_util::{js, json};
use serde_json::Value;

use crate::jsx;
use crate::omap::OrderedMap;
use crate::title::Title;
use crate::{Clocks, LogFn, files};

const MAX_PERSONAS: usize = 100_000;
const MAX_BEST: usize = 300;
/// the largest figure a record takes (a report's figure past it is no car's): top speed in m/s (540 km/h), drift
/// points, powerslide in m, jump in s
const RECORD_MAX: [f64; 4] = [150.0, 100_000_000.0, 10_000.0, 60.0];

/// one race type's counts (f64: a hand-edited file's numbers are kept, fractions too, from 0 to 2^31)
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Counts {
    pub races: f64,
    pub rw: f64,
    pub rl: f64,
    pub uw: f64,
    pub ul: f64,
    pub points: f64,
    pub dc: f64,
}

/// the counts by race type, by the title's numbers (in the file an object of the types raced,
/// `{ "0": {...}, "3": {...} }`, in key order)
pub type Table = [Option<Counts>; 6];

/// a persona's records, as its Personal Stats screen shows them (0: none yet): top speed (m/s), biggest drift points,
/// longest powerslide (m), longest jump (s), quickest drag finished (s).  Overall only: the screens read STAT, not the
/// month's LMSTAT (SPEED2.EXE 0x4d3490, 0x4be680).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Records {
    pub top: f64,
    pub drift: f64,
    pub slide: f64,
    pub jump: f64,
    pub drag: f64,
}

/// a persona's record: its name, the month it was last seen, its counts overall (all) and for the month, its best
/// times ("type:track" -> seconds), its records
#[derive(Clone, Debug)]
pub struct Record {
    pub name: String,
    pub seen: String,
    pub all: Table,
    pub month_key: String,
    pub month: Table,
    pub best: IndexMap<String, f64>,
    pub records: Records,
}

/// a player of a race: place 1..n, 0 = did not finish; time in seconds
#[derive(Clone, Debug)]
pub struct RacePlayer {
    pub name: String,
    pub place: u32,
    pub time: f64,
}

/// one race's result: type (the title's race_types), ranked, track, the players in finishing order
#[derive(Clone, Debug)]
pub struct Race {
    pub kind: u32,
    pub ranked: bool,
    pub track: i64,
    pub players: Vec<RacePlayer>,
}

/// a row of a ranking
#[derive(Clone, Debug, PartialEq)]
pub struct BoardRow {
    pub name: String,
    pub points: f64,
    pub wins: f64,
    pub losses: f64,
    pub races: f64,
    pub rank: usize,
}

/// a ranking as last built: the rows with their ranks, and the rows by lower-case name
pub struct Ranking {
    t: f64,
    month: String,
    pub rows: Vec<BoardRow>,
    pub index: HashMap<String, usize>,
}

struct Sorted {
    t: f64,
    stale: bool,
    names: Vec<(String, String)>,
}

/// what a Stats is given (the server's: `StatsOptions::new`; the tests change them)
pub struct StatsOptions {
    /// where stats.json lives (None: in memory only, for the tests)
    pub dir: Option<PathBuf>,
    pub clocks: Rc<Clocks>,
    pub log: LogFn,
    pub save_ms: f64,
    pub prune_months: i64,
    pub max: usize,
    pub board_ms: f64,
    /// the game's race types and STAT field (NFS Underground 2's unless changed)
    pub title: &'static Title,
}

impl StatsOptions {
    pub fn new(dir: Option<PathBuf>, clocks: Rc<Clocks>, log: LogFn) -> Self {
        StatsOptions {
            dir,
            clocks,
            log,
            save_ms: 30_000.0,
            prune_months: 12,
            max: MAX_PERSONAS,
            board_ms: 5000.0,
            title: &crate::title::UG2,
        }
    }
}

pub struct Stats {
    dir: Option<PathBuf>,
    clocks: Rc<Clocks>,
    log: LogFn,
    save_ms: f64,
    prune_months: i64,
    max: usize,
    board_ms: f64,
    title: &'static Title,
    /// lower-case name -> record, the least recently seen first
    p: OrderedMap<String, Record>,
    /// (race type, monthly) -> the ranking as last built
    boards: HashMap<(u32, bool), Ranking>,
    /// the names sorted by their lower case (EA Messenger's search), and when
    sorted: Option<Sorted>,
    dirty: bool,
    /// when the debounced save is due (the timer clock), if one is armed
    save_due: Option<f64>,
    /// how often a board was built (the tests: the rankings are built once for many asks)
    pub builds: usize,
}

fn blank() -> Counts {
    Counts::default()
}

impl Stats {
    pub fn new(o: StatsOptions) -> Stats {
        let mut s = Stats {
            dir: o.dir,
            clocks: o.clocks,
            log: o.log,
            save_ms: o.save_ms,
            prune_months: o.prune_months,
            max: o.max,
            board_ms: o.board_ms,
            title: o.title,
            p: OrderedMap::new(),
            boards: HashMap::new(),
            sorted: None,
            dirty: false,
            save_due: None,
            builds: 0,
        };
        if s.dir.is_some() {
            s.load();
        }
        s
    }

    fn now(&self) -> f64 {
        (self.clocks.now)()
    }

    pub fn file(&self) -> Option<PathBuf> {
        self.dir.as_ref().map(|d| d.join("stats.json"))
    }

    fn load(&mut self) {
        let Some(file) = self.file() else { return };
        let Ok(bytes) = std::fs::read(&file) else { return };
        let text = String::from_utf8_lossy(&bytes);
        match crate::jsonparse::parse(&text) {
            Ok(j) => {
                let mut rows: Vec<Record> = match j.get("personas") {
                    Some(Value::Array(a)) => a
                        .iter()
                        .filter_map(|r| {
                            let name = r.get("name")?.as_str()?;
                            // one kept before names were checked for slurs (nfsu2-names): the server takes no such
                            // name now
                            (jsx::valid_name(name) && !nfsu2_names::blocked_name(name)).then(|| clean(r, name))
                        })
                        .collect(),
                    _ => Vec::new(),
                };
                rows.sort_by(|a, b| a.seen.cmp(&b.seen));
                let from = if self.max == 0 { 0 } else { rows.len().saturating_sub(self.max) };
                for r in rows.drain(from..) {
                    self.p.insert(r.name.to_lowercase(), r);
                }
                (self.log)(&format!("[online] results of {} personas read from {}", self.p.len(), file.display()), None);
            }
            Err(e) => {
                let aside = format!("{}.bad-{}", file.display(), nfsu2_util::now_ms());
                let _ = std::fs::rename(&file, &aside);
                (self.log)(
                    &format!("[online] {} could not be read ({e}): moved to {aside}, starting with no results", file.display()),
                    None,
                );
            }
        }
    }

    /// a persona's record (made when asked for by a race: the least recently seen goes when there are max)
    pub fn rec(&mut self, name: &str, make: bool) -> Option<&mut Record> {
        let k = jsx::lower(name);
        if !self.p.contains_key(&k) && make {
            while self.p.len() >= self.max {
                let Some(old) = self.p.first_key().cloned() else { break };
                self.p.remove(&old);
            }
            let m = jsx::month_of(self.now());
            let r = Record {
                name: name.to_string(),
                seen: m.clone(),
                all: Default::default(),
                month_key: m,
                month: Default::default(),
                best: IndexMap::new(),
                records: Records::default(),
            };
            self.p.insert(k.clone(), r);
            if let Some(s) = &mut self.sorted {
                s.stale = true;
            }
        }
        let m = jsx::month_of(self.now());
        let r = self.p.get_mut(&k)?;
        roll(r, &m);
        Some(r)
    }

    /// the number of records
    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.p.len()
    }

    /// One race's result: each player's counts and points (overall and for the month, `race_points`), best time,
    /// month seen.
    pub fn add_race(&mut self, race: &Race) {
        let n = race.players.len();
        if n < 2 || race.kind as usize >= self.title.race_types.len() {
            return;
        }
        let t = race.kind as usize;
        let m = jsx::month_of(self.now());
        for pl in &race.players {
            let k = jsx::lower(&pl.name);
            if self.rec(&pl.name, true).is_none() {
                continue;
            }
            // r raced now: the most recently seen
            self.p.move_to_end(&k);
            let r = self.p.get_mut(&k).expect("just made");
            r.seen = m.clone();
            let won = pl.place == 1;
            let pts = race_points(race.ranked, n, pl.place);
            for table in [&mut r.all, &mut r.month] {
                let c = table[t].get_or_insert_with(blank);
                c.races += 1.0;
                c.points += pts;
                match (race.ranked, won) {
                    (true, true) => c.rw += 1.0,
                    (true, false) => c.rl += 1.0,
                    (false, true) => c.uw += 1.0,
                    (false, false) => c.ul += 1.0,
                }
                // dropped out of the race
                if pl.place == 0 {
                    c.dc += 1.0;
                }
            }
            if pl.place != 0 && pl.time > 0.0 {
                let key = format!("{t}:{}", race.track);
                let old = r.best.get(&key).copied();
                if !old.is_some_and(|b| b <= pl.time) && (old.is_some() || r.best.len() < MAX_BEST) {
                    r.best.insert(key, jsx::round(pl.time * 1000.0) / 1000.0);
                }
            }
        }
        self.touch();
    }

    /// A player's own entry in its race report (lobby.rs: one taken report per player, the player finished): each
    /// figure that beats the persona's record becomes it.  A figure no car makes (`RECORD_MAX`: a top speed over
    /// 540 km/h ...) is not taken; quickest drag is a drag race's time (`check_result` checked it).
    pub fn add_records(&mut self, name: &str, kind: u32, p: &ReportPlayer) {
        let fits = |x: f64, max: f64| x.is_finite() && x > 0.0 && x <= max;
        let x = [p.top, p.drift_best as f64, p.slide, p.jump];
        let drag = (self.title.race_types.get(kind as usize) == Some(&"drag") && fits(p.time, f64::MAX)).then_some(p.time);
        // (nothing to keep: no record made for it)
        if drag.is_none() && !x.iter().zip(RECORD_MAX).any(|(x, max)| fits(*x, max)) {
            return;
        }
        let Some(r) = self.rec(name, true) else { return };
        let old = r.records;
        let rec = &mut r.records;
        for ((x, max), kept) in x.into_iter().zip(RECORD_MAX).zip([&mut rec.top, &mut rec.drift, &mut rec.slide, &mut rec.jump]) {
            if fits(x, max) && x > *kept {
                *kept = jsx::round(x * 1000.0) / 1000.0;
            }
        }
        if let Some(t) = drag
            && (rec.drag == 0.0 || t < rec.drag)
        {
            rec.drag = jsx::round(t * 1000.0) / 1000.0;
        }
        if r.records != old {
            self.touch();
        }
    }

    /// The game's STAT field of a persona (the title's `stat`: overall, with its records, or the month's, LMSTAT,
    /// without), all zero for one with no record.
    pub fn stat_field(&mut self, name: Option<&str>, monthly: bool) -> String {
        let (table, rec): (Table, Records) = match name.filter(|n| !n.is_empty()).and_then(|n| self.rec(n, false)) {
            Some(r) => {
                if monthly {
                    (r.month, Records::default())
                } else {
                    (r.all, r.records)
                }
            }
            None => Default::default(),
        };
        (self.title.stat)(&table, &rec)
    }

    /// a ranking: best first (points, then wins, then fewer losses, then name)
    pub fn board(&mut self, kind: u32, monthly: bool) -> Vec<BoardRow> {
        self.builds += 1;
        let m = jsx::month_of(self.now());
        let mut rows = Vec::new();
        let t = kind as usize;
        self.p.for_each_mut(|_, r| {
            roll(r, &m);
            let table = if monthly { &r.month } else { &r.all };
            let Some(c) = table.get(t).copied().flatten() else { return };
            if c.races == 0.0 {
                return;
            }
            rows.push(BoardRow { name: r.name.clone(), points: c.points, wins: c.rw + c.uw, losses: c.rl + c.ul, races: c.races, rank: 0 });
        });
        rows.sort_by(|a, b| {
            b.points
                .partial_cmp(&a.points)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then(b.wins.partial_cmp(&a.wins).unwrap_or(std::cmp::Ordering::Equal))
                .then(a.losses.partial_cmp(&b.losses).unwrap_or(std::cmp::Ordering::Equal))
                .then_with(|| a.name.cmp(&b.name))
        });
        rows
    }

    /// a ranking as the game's Rankings ask for it: the board with each row's rank, and the rows by name; built at
    /// most every board_ms (a race's result shows within that)
    pub fn ranking(&mut self, kind: u32, monthly: bool) -> &Ranking {
        let now = self.now();
        let month = jsx::month_of(now);
        let fresh = self.boards.get(&(kind, monthly)).is_some_and(|b| now - b.t < self.board_ms && b.month == month);
        if !fresh {
            let mut rows = self.board(kind, monthly);
            for (i, r) in rows.iter_mut().enumerate() {
                r.rank = i + 1;
            }
            let index = rows.iter().enumerate().map(|(i, r)| (r.name.to_lowercase(), i)).collect();
            self.boards.insert((kind, monthly), Ranking { t: now, month, rows, index });
        }
        &self.boards[&(kind, monthly)]
    }

    /// at most max names that begin with prefix (lower case), from a sorted list built at most every board_ms
    pub fn starting_with(&mut self, prefix: &str, max: usize) -> Vec<String> {
        let now = self.now();
        if self.sorted.as_ref().is_none_or(|s| s.stale && now - s.t >= self.board_ms) {
            let mut names: Vec<(String, String)> = self.p.values().map(|r| (r.name.to_lowercase(), r.name.clone())).collect();
            names.sort_by(|a, b| a.0.cmp(&b.0));
            self.sorted = Some(Sorted { t: now, stale: false, names });
        }
        let n = &self.sorted.as_ref().expect("built").names;
        let lo = n.partition_point(|x| x.0.as_str() < prefix);
        let mut out = Vec::new();
        for (low, name) in &n[lo..] {
            if out.len() >= max || !low.starts_with(prefix) {
                break;
            }
            if self.p.contains_key(low) {
                out.push(name.clone());
            }
        }
        out
    }

    fn touch(&mut self) {
        self.dirty = true;
        if self.dir.is_none() || self.save_due.is_some() {
            return;
        }
        self.save_due = Some((self.clocks.timer)() + self.save_ms);
    }

    /// when the debounced save is due (the timer clock)
    pub fn save_due(&self) -> Option<f64> {
        self.save_due
    }

    /// the save timer fired
    pub fn save_timer(&mut self) {
        self.save_due = None;
        self.save();
    }

    fn prune(&mut self) {
        let m = jsx::month_of(self.now());
        let (y, mo) = month_parts(&m);
        let cut = y * 12 + mo - self.prune_months;
        let old: Vec<String> = self
            .p
            .iter()
            .filter(|(_, r)| {
                let (ry, rm) = month_parts(&r.seen);
                ry * 12 + rm < cut
            })
            .map(|(k, _)| k.clone())
            .collect();
        for k in old {
            self.p.remove(&k);
        }
    }

    /// the file's text: version 1, every record, field for field in the Node server's format
    pub fn serialize(&self) -> String {
        let mut s = String::with_capacity(64 + 160 * self.p.len());
        s.push_str("{\"v\":1,\"note\":\"NFSU2 online results per persona (public in the game)\",\"personas\":[");
        for (i, r) in self.p.values().enumerate() {
            if i > 0 {
                s.push(',');
            }
            s.push_str("{\"name\":");
            json::write_str(&mut s, &r.name);
            s.push_str(",\"seen\":");
            json::write_str(&mut s, &r.seen);
            s.push_str(",\"all\":");
            write_table(&mut s, &r.all);
            s.push_str(",\"month\":{\"key\":");
            json::write_str(&mut s, &r.month_key);
            s.push_str(",\"t\":");
            write_table(&mut s, &r.month);
            s.push_str("},\"best\":{");
            for (j, (k, v)) in r.best.iter().enumerate() {
                if j > 0 {
                    s.push(',');
                }
                json::write_str(&mut s, k);
                s.push(':');
                s.push_str(&num(*v));
            }
            s.push('}');
            // (a persona with none: as the Node server wrote it)
            if r.records != Records::default() {
                s.push_str(",\"rec\":{");
                let mut first = true;
                for (k, v) in rec_fields(&r.records) {
                    if v == 0.0 {
                        continue;
                    }
                    if !first {
                        s.push(',');
                    }
                    first = false;
                    s.push_str(&format!("\"{k}\":{}", num(v)));
                }
                s.push('}');
            }
            s.push('}');
        }
        s.push_str("]}");
        s
    }

    pub fn save(&mut self) {
        let Some(dir) = self.dir.clone() else { return };
        if !self.dirty {
            return;
        }
        self.prune();
        let body = self.serialize();
        match files::write_atomic(&dir, &dir.join("stats.json"), body.as_bytes()) {
            Ok(()) => self.dirty = false,
            Err(e) => (self.log)(&format!("[online] the results could not be written: {e}"), None),
        }
    }

    /// the server stops: the timer ends, the results are written
    pub fn stop(&mut self) {
        self.save_due = None;
        self.save();
    }
}

/// a new month: the monthly counts start again
fn roll(r: &mut Record, m: &str) {
    if r.month_key != m {
        r.month_key = m.to_string();
        r.month = Default::default();
    }
}

/// `s.split('-').map(Number)` of a month that passed its check ("2026-10")
fn month_parts(s: &str) -> (i64, i64) {
    let mut it = s.split('-').map(|x| jsx::to_number(x) as i64);
    (it.next().unwrap_or(0), it.next().unwrap_or(0))
}

fn num(x: f64) -> String {
    // a finite number as JSON.stringify writes it (-0 as 0), else null
    if x.is_finite() { js::number_to_string(x) } else { "null".into() }
}

fn write_table(s: &mut String, t: &Table) {
    s.push('{');
    let mut first = true;
    for (i, c) in t.iter().enumerate() {
        let Some(c) = c else { continue };
        if !first {
            s.push(',');
        }
        first = false;
        s.push_str(&format!(
            "\"{i}\":{{\"races\":{},\"rw\":{},\"rl\":{},\"uw\":{},\"ul\":{},\"points\":{},\"dc\":{}}}",
            num(c.races),
            num(c.rw),
            num(c.rl),
            num(c.uw),
            num(c.ul),
            num(c.points),
            num(c.dc)
        ));
    }
    s.push('}');
}

/// a parsed JSON value that counts as there: not null, false, 0, NaN or ""
fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().is_some_and(|x| x != 0.0 && !x.is_nan()),
        Value::String(s) => !s.is_empty(),
        _ => true,
    }
}

/// a count from the file: a finite number of at least 0, at most 2^31; anything else is 0
fn count(v: Option<&Value>) -> f64 {
    match v.and_then(Value::as_f64) {
        Some(x) if x.is_finite() && x >= 0.0 => x.min(2_147_483_648.0),
        _ => 0.0,
    }
}

/// a record's counts from the file: each race type's entry (an object's "0".."5"; an array's elements and a string's
/// characters too, as the Node server read them), one that is not there (`truthy`) skipped, the numbers checked
fn counts(t: Option<&Value>) -> Table {
    // a string's character: truthy, with none of the fields
    static CHARACTER: Value = Value::Bool(true);
    let mut o: Table = Default::default();
    let Some(t) = t else { return o };
    for (i, slot) in o.iter_mut().enumerate() {
        // (borrowed, never cloned: a value of the file may be nested deeper than a stack, jsonparse.rs `Parsed`)
        let c = match t {
            Value::Object(m) => m.get(&i.to_string()),
            Value::Array(a) => a.get(i),
            Value::String(s) => (s.encode_utf16().count() > i).then_some(&CHARACTER),
            _ => None,
        };
        let Some(c) = c else { continue };
        if !truthy(c) {
            continue;
        }
        let f = |k: &str| count(if let Value::Object(m) = c { m.get(k) } else { None });
        *slot = Some(Counts { races: f("races"), rw: f("rw"), rl: f("rl"), uw: f("uw"), ul: f("ul"), points: f("points"), dc: f("dc") });
    }
    o
}

/// a best time's key: one digit (the race type), ':', 1 to 6 digits (the track)
fn best_key(k: &str) -> bool {
    let b = k.as_bytes();
    b.len() >= 3 && b.len() <= 8 && b[0].is_ascii_digit() && b[1] == b':' && b[2..].iter().all(u8::is_ascii_digit)
}

/// A stored record, with only the fields this file writes (numbers checked).  (A `seen` or `month.key` that is not a
/// string counts as none: the Node server took one whose text read as a month, an array ["2026-10"], and then failed
/// to write the file.)
fn clean(r: &Value, name: &str) -> Record {
    let mut best = IndexMap::new();
    if let Some(Value::Object(b)) = r.get("best") {
        for (k, v) in b {
            if let Some(x) = v.as_f64()
                && best_key(k)
                && x.is_finite()
                && x > 0.0
            {
                best.insert(k.clone(), x);
            }
        }
    }
    let month_str = |v: Option<&Value>| v.and_then(Value::as_str).filter(|s| jsx::is_month(s)).map(str::to_string);
    let seen = month_str(r.get("seen")).unwrap_or_else(|| "1970-01".into());
    let month = r.get("month");
    let key = month_str(month.and_then(|m| if m.is_object() { m.get("key") } else { None })).unwrap_or_else(|| seen.clone());
    let mt = month.and_then(|m| if m.is_object() { m.get("t") } else { None });
    let all = counts(r.get("all").filter(|_| r.is_object()));
    let mut rec = Records::default();
    if let Some(Value::Object(o)) = r.get("rec") {
        let take = |k: &str, max: f64| match o.get(k).and_then(Value::as_f64) {
            Some(x) if x.is_finite() && x > 0.0 && x <= max => x,
            _ => 0.0,
        };
        let [top, drift, slide, jump] = RECORD_MAX;
        rec = Records {
            top: take("top", top),
            drift: take("drift", drift),
            slide: take("slide", slide),
            jump: take("jump", jump),
            drag: take("drag", 1e9),
        };
    }
    Record { name: name.to_string(), seen, all, month_key: key, month: counts(mt), best, records: rec }
}

/// a record's figures by their names in the file
fn rec_fields(r: &Records) -> [(&'static str, f64); 5] {
    [("top", r.top), ("drift", r.drift), ("slide", r.slide), ("jump", r.jump), ("drag", r.drag)]
}

/// a player's rating points for one race of `n` players (place 1..n, 0 = did not finish): (n - place) * 100 / (n - 1),
/// so the winner gets 100 and the last 0, ranked races only
pub fn race_points(ranked: bool, n: usize, place: u32) -> f64 {
    if ranked && n >= 2 && place != 0 { jsx::round((n as f64 - place as f64) * 100.0 / (n as f64 - 1.0)) } else { 0.0 }
}

/// The 38 numbers of NFS Underground 2's STAT field (hex, each followed by a comma), as its Personal Stats screen reads
/// them (found by sending distinct values; SPEED2.EXE 0x4d3490 and its rows' units): [0] not shown, [1] client
/// disconnects, [2] host disconnects (both shown as a share of the games), [3] top speed (mm/s), [4] biggest drift
/// points, [5] longest powerslide (mm), [6] longest jump (ms), [7] quickest drag (ms) (the records: STAT only); then
/// per race type rating, ranked wins, ranked losses, unranked wins, unranked losses (Games, Wins and Losses on the
/// screen are their sums).
pub fn stat_v38(table: &Table, rec: &Records) -> String {
    let mut dc = 0.0;
    for c in table.iter().flatten() {
        dc += c.dc;
    }
    let milli = |x: f64| jsx::round(x * 1000.0);
    let mut v = vec![0.0, dc, 0.0, milli(rec.top), rec.drift, milli(rec.slide), milli(rec.jump), milli(rec.drag)];
    for c in table {
        let c = c.unwrap_or_default();
        v.extend([c.points, c.rw, c.rl, c.uw, c.ul]);
    }
    let mut s = String::with_capacity(v.len() * 3);
    for x in v {
        s.push_str(&format!("{:x},", jsx::to_uint32(x)));
    }
    s
}

/// The game's race report (rank RESU, base64), version 3 as SPEED2.EXE writes it:
///   [0] version 3, [1] players, [2] race type, [3] the reporter's index, [4..5] track (big-endian), [6] direction,
///   [7] laps; then per player 55 bytes: [0] index into the report's NAME list, [1] place, [2] 0x01 finished / 0x0c
///   dropped out, [3] the car, [8..11] the total time in seconds (float, big-endian; a dropped player's: until it
///   left), [12..15] the best lap, [16..19] average speed, [20..23] top speed (m/s), [24..27] 0-60 time,
///   [28..31] quarter mile time (s), [32..35] quarter mile speed (m/s; -1.0 for a dropped player, the research says
///   [44..47]: both are taken), [36..39] longest jump (s: the longest run of the car's drift-score style 35, more than
///   0.75 m off the ground, SPEED2.EXE 0x56f0b1, kept by 0x56d500), [40..43] longest powerslide (m: style 34's
///   longest run of the car's distance, 0x56f5fb), [44..47] the drift score (an integer: the racer's points after its
///   last lap, what the game's own results show for drift; 0x5fec5d), [48..51] the biggest single drift's points (an
///   integer, the drift style's best), [53] a bit mask, each set bit adding 2 bytes after the record (one byte
///   here: UNVERIFIED)
/// (from the builder, 0x5fe8d0, and the reports of real races here; the fields not used are UNVERIFIED).  A player's
/// figures are measured by the reporter's game: its own are what it raced, the others' what it saw of them.
#[derive(Clone, Debug)]
#[allow(dead_code)] // decoded whole (the report as the game wrote it), read in part
pub struct RaceReport {
    pub kind: u8,
    pub reporter: u8,
    pub track: u16,
    pub dir: u8,
    pub laps: u8,
    pub players: Vec<ReportPlayer>,
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
pub struct ReportPlayer {
    pub index: u8,
    pub place: u8,
    pub car: u8,
    pub time: f64,
    pub score: i32,
    pub dropped: bool,
    pub top: f64,
    /// its average speed (m/s): times its time, the way it drove
    pub avg: f64,
    pub jump: f64,
    pub slide: f64,
    pub drift_best: i32,
}

/// a race report from its base64 (rank RESU): None for anything that does not parse
pub fn decode_result(b64: &str) -> Option<RaceReport> {
    let b = node_base64(b64);
    if b.len() < 8 || b[0] != 3 {
        return None;
    }
    let n = b[1] as usize;
    if !(1..=8).contains(&n) {
        return None;
    }
    let f32_at = |o: usize| f32::from_be_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]) as f64;
    let i32_at = |o: usize| i32::from_be_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
    let finite = |x: f64| if x.is_finite() { x } else { 0.0 };
    let mut players = Vec::with_capacity(n);
    let mut o = 8;
    for _ in 0..n {
        if o + 55 > b.len() {
            return None;
        }
        let extra = 2 * (b[o + 53].count_ones() as usize);
        // dropped out: the flag byte 0x0c instead of 0x01 and -1.0 at 32 (a four-player race here), or -1.0 at 44
        // (the research)
        let time = f32_at(o + 8);
        let dropped = (b[o + 2] & 0x08) != 0 || f32_at(o + 32) == -1.0 || f32_at(o + 44) == -1.0;
        players.push(ReportPlayer {
            index: b[o],
            place: b[o + 1],
            car: b[o + 3],
            time: finite(time),
            score: i32_at(o + 44),
            dropped,
            top: finite(f32_at(o + 20)),
            avg: finite(f32_at(o + 16)),
            jump: finite(f32_at(o + 36)),
            slide: finite(f32_at(o + 40)),
            drift_best: i32_at(o + 48),
        });
        o += 55 + extra;
    }
    Some(RaceReport { kind: b[2], reporter: b[3], track: u16::from_be_bytes([b[4], b[5]]), dir: b[6], laps: b[7], players })
}

/// base64 as Node's `Buffer.from(s, 'base64')` reads it (a report the Node server took is taken): both alphabets,
/// other characters skipped, the first "=" ends it, the bits of an unfinished last byte dropped
pub fn node_base64(s: &str) -> Vec<u8> {
    let mut o = Vec::with_capacity(s.len() * 3 / 4);
    let (mut acc, mut bits) = (0u32, 0u32);
    for c in s.chars() {
        let v = match c {
            'A'..='Z' => c as u32 - 'A' as u32,
            'a'..='z' => c as u32 - 'a' as u32 + 26,
            '0'..='9' => c as u32 - '0' as u32 + 52,
            '+' | '-' => 62,
            '/' | '_' => 63,
            '=' => break,
            _ => continue,
        };
        acc = (acc << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            o.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    o
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use std::cell::Cell;

    pub fn clocks(t: Rc<Cell<f64>>) -> Rc<Clocks> {
        let t2 = t.clone();
        Rc::new(Clocks { now: Box::new(move || t.get()), timer: Box::new(move || t2.get()) })
    }

    fn quiet() -> LogFn {
        Rc::new(|_: &str, _: Option<&str>| {})
    }

    fn race(kind: u32, ranked: bool, track: i64, players: &[(&str, u32, f64)]) -> Race {
        Race {
            kind,
            ranked,
            track,
            players: players.iter().map(|(n, p, t)| RacePlayer { name: n.to_string(), place: *p, time: *t }).collect(),
        }
    }

    const T0: f64 = 1_792_411_200_000.0; // Date.UTC(2026, 9, 19, 12)

    fn hexes(s: &str) -> Vec<u32> {
        s.split(',').filter(|x| !x.is_empty()).map(|x| u32::from_str_radix(x, 16).unwrap()).collect()
    }

    // counts, points, STAT, boards, months
    #[test]
    fn counts_points_stat_boards_months() {
        let t = Rc::new(Cell::new(T0));
        let mut st = Stats::new(StatsOptions::new(None, clocks(t.clone()), quiet()));
        st.add_race(&race(0, true, 4014, &[("Alpha", 1, 100.0), ("Bravo", 2, 101.0), ("Charlie", 0, 0.0)]));
        st.add_race(&race(0, false, 4014, &[("Bravo", 1, 99.5), ("Alpha", 2, 102.0)]));
        let b = st.board(0, false);
        let got: Vec<(String, f64, f64, f64, f64)> = b.iter().map(|r| (r.name.clone(), r.points, r.wins, r.losses, r.races)).collect();
        assert_eq!(
            got,
            [
                ("Alpha".to_string(), 100.0, 1.0, 1.0, 2.0),
                ("Bravo".to_string(), 50.0, 1.0, 1.0, 2.0),
                ("Charlie".to_string(), 0.0, 0.0, 1.0, 1.0)
            ]
        );
        let stat = st.stat_field(Some("alpha"), false);
        assert_eq!(stat.split(',').count(), 39);
        assert!(stat.ends_with(','));
        assert_eq!(hexes(&stat)[0..8], [0; 8]);
        assert_eq!(hexes(&st.stat_field(Some("Charlie"), false))[0..2], [0, 1], "a dropped race: a disconnect");
        assert_eq!(hexes(&stat)[8..13], [100, 1, 0, 0, 1]);
        assert_eq!(st.rec("Bravo", false).unwrap().best["0:4014"], 99.5);
        assert_eq!(st.stat_field(Some("nobody"), false), "0,".repeat(38));
        // a new month: the monthly counts start again, the overall ones stay
        t.set(1_793_577_600_000.0); // Date.UTC(2026, 10, 2)
        assert_eq!(st.board(0, true).len(), 0);
        assert_eq!(st.board(0, false).len(), 3);
        assert_eq!(st.stat_field(Some("Alpha"), true), "0,".repeat(38));
        st.add_race(&race(1, false, 1, &[("Alpha", 1, 50.0), ("Bravo", 2, 51.0)]));
        assert_eq!(st.board(1, true)[0].name, "Alpha");
    }

    // saved atomically with public fields only, read back, a damaged file kept aside, pruned
    #[test]
    fn saved_read_back_damaged_pruned() {
        let dir = tempfile::tempdir().unwrap();
        let d = dir.path().to_path_buf();
        let t = Rc::new(Cell::new(T0));
        let opts = |log: LogFn| {
            let mut o = StatsOptions::new(Some(d.clone()), clocks(t.clone()), log);
            o.save_ms = 60_000.0;
            o
        };
        let mut a = Stats::new(opts(quiet()));
        a.add_race(&race(2, true, 7, &[("Alpha", 1, 10.0), ("Bravo", 2, 11.0)]));
        a.stop();
        let text = std::fs::read_to_string(d.join("stats.json")).unwrap();
        let names: Vec<String> = std::fs::read_dir(&d).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).collect();
        assert_eq!(names, ["stats.json"], "no temporary file left");
        let j: Value = serde_json::from_str(&text).unwrap();
        let mut keys: Vec<&String> = j["personas"][0].as_object().unwrap().keys().collect();
        keys.sort();
        assert_eq!(keys, ["all", "best", "month", "name", "seen"]);
        assert!(
            !text.contains("10.77.") && !text.contains("PASS") && !text.contains("MID") && !text.contains("ADDR") && !text.contains('@')
        );
        let mut b = Stats::new(opts(quiet()));
        let got: Vec<(String, f64)> = b.board(2, false).iter().map(|r| (r.name.clone(), r.points)).collect();
        assert_eq!(got, [("Alpha".to_string(), 100.0), ("Bravo".to_string(), 0.0)]);
        // a hand-edited file: unknown fields and bad numbers are dropped
        std::fs::write(
            d.join("stats.json"),
            r#"{"personas":[{"name":"Evil<script>","all":{}},{"name":"Okay","seen":"2026-10","all":{"0":{"races":-5,"rw":"x","points":7,"secret":"y"}},"ip":"1.2.3.4"}]}"#,
        )
        .unwrap();
        let mut c = Stats::new(opts(quiet()));
        assert!(c.rec("Evil<script>", false).is_none());
        assert_eq!(c.rec("Okay", false).unwrap().all[0], Some(Counts { points: 7.0, ..Default::default() }));
        // damaged: moved aside, a fresh start
        std::fs::write(d.join("stats.json"), "{not json").unwrap();
        let logs = Rc::new(std::cell::RefCell::new(Vec::<String>::new()));
        let l2 = logs.clone();
        let mut e = Stats::new(opts(Rc::new(move |s: &str, _: Option<&str>| l2.borrow_mut().push(s.to_string()))));
        assert_eq!(e.board(0, false).len(), 0);
        assert!(std::fs::read_dir(&d).unwrap().any(|f| f.unwrap().file_name().to_string_lossy().starts_with("stats.json.bad-")));
        assert!(logs.borrow().iter().any(|l| l.contains("could not be read")));
        // personas not seen for a year are forgotten at the next save
        e.add_race(&race(0, false, 1, &[("Old", 1, 5.0), ("Older", 2, 6.0)]));
        t.set(1_827_705_600_000.0); // Date.UTC(2027, 11, 1)
        e.add_race(&race(0, false, 1, &[("New", 1, 5.0), ("Newer", 2, 6.0)]));
        e.stop();
        let j: Value = serde_json::from_str(&std::fs::read_to_string(d.join("stats.json")).unwrap()).unwrap();
        let mut names: Vec<String> = j["personas"].as_array().unwrap().iter().map(|p| p["name"].as_str().unwrap().to_string()).collect();
        names.sort();
        assert_eq!(names, ["New", "Newer"]);
    }

    // a persona kept from before names were checked for slurs is dropped as the file is read; the others stay (the
    // Node server's Stats kept only "speedy" of this file, as here)
    #[test]
    fn a_name_that_reads_as_a_slur_is_dropped_from_the_file() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(
            dir.path().join("stats.json"),
            r#"{"v":1,"personas":[{"name":"Speedy","seen":1792411200000},{"name":"F4g_42","seen":1792411200000}]}"#,
        )
        .unwrap();
        let st = Stats::new(StatsOptions::new(Some(dir.path().to_path_buf()), clocks(Rc::new(Cell::new(T0))), quiet()));
        assert_eq!(st.p.keys().collect::<Vec<_>>(), ["speedy"]);
    }

    // race report decoder
    #[test]
    fn race_report_decoder() {
        let r = decode_result("AwIAAQ+uAAIBAQEVQAADJ0M60exCuGZnQflKqkIogLdAsZmaQU2tD0I5HwoAAAAAAAAAAAAAAAAAAAAAAAAAAAIBFUAAAOhDO6uGQreo9kH44idCKNlYQL3CkEFciThCPgV4AAAAAAAAAAAAAAAAAAAAAAAAAA==").unwrap();
        assert_eq!((r.kind, r.track, r.laps, r.players.len()), (0, 4014, 2, 2));
        let p: Vec<(u8, u8, f64, bool)> =
            r.players.iter().map(|p| (p.index, p.place, (p.time * 100.0).round() / 100.0, p.dropped)).collect();
        assert_eq!(p, [(1, 1, 186.82, false), (0, 2, 187.67, false)]);
        let tops: Vec<f64> = r.players.iter().map(|p| (p.top * 1000.0).round() / 1000.0).collect();
        assert_eq!(tops, [42.126, 42.212], "top speeds in m/s (152 km/h)");
        assert!(r.players.iter().all(|p| p.jump == 0.0 && p.slide == 0.0 && p.drift_best == 0), "no jump, slide or drift");
        assert!(decode_result("not base64 at all!").is_none());
        let b64 = |b: &[u8]| {
            use base64::Engine;
            base64::engine::general_purpose::STANDARD.encode(b)
        };
        assert!(decode_result(&b64(&[2, 1, 0, 0, 0, 0, 0, 0])).is_none(), "another version");
        assert!(decode_result(&b64(&[3, 2, 0, 0, 0, 0, 0, 0, 1, 1])).is_none(), "cut short");
        let mut dropped = vec![0u8; 8 + 55 * 2];
        dropped[0] = 3;
        dropped[1] = 2;
        dropped[8] = 0;
        dropped[9] = 1;
        dropped[16..20].copy_from_slice(&90f32.to_be_bytes());
        dropped[8 + 2] = 1;
        dropped[63] = 1;
        dropped[64] = 2;
        dropped[65] = 0x0c;
        dropped[63 + 32..63 + 36].copy_from_slice(&(-1f32).to_be_bytes());
        let d: Vec<bool> = decode_result(&b64(&dropped)).unwrap().players.iter().map(|p| p.dropped).collect();
        assert_eq!(d, [false, true]);
        dropped[65] = 1;
        dropped[63 + 32..63 + 36].copy_from_slice(&0f32.to_be_bytes());
        dropped[63 + 44..63 + 48].copy_from_slice(&(-1f32).to_be_bytes());
        let d: Vec<bool> = decode_result(&b64(&dropped)).unwrap().players.iter().map(|p| p.dropped).collect();
        assert_eq!(d, [false, true]);
        // the records' figures: jump [36..39], powerslide [40..43] (floats), biggest drift [48..51] (an integer)
        dropped[8 + 36..8 + 40].copy_from_slice(&1.5f32.to_be_bytes());
        dropped[8 + 40..8 + 44].copy_from_slice(&20.25f32.to_be_bytes());
        dropped[8 + 48..8 + 52].copy_from_slice(&70_000i32.to_be_bytes());
        let p = &decode_result(&b64(&dropped)).unwrap().players[0];
        assert_eq!((p.jump, p.slide, p.drift_best), (1.5, 20.25, 70_000));
    }

    // records: a player's own figures, the best kept (a figure no car makes is not), quickest drag from drag races
    // only, in STAT [3]..[7] (none in the month's LMSTAT), saved and read back
    #[test]
    fn records() {
        let dir = tempfile::tempdir().unwrap();
        let t = Rc::new(Cell::new(T0));
        let opts = || StatsOptions::new(Some(dir.path().to_path_buf()), clocks(t.clone()), quiet());
        let mut st = Stats::new(opts());
        let me = |time, top, jump, slide, drift_best| ReportPlayer {
            index: 0,
            place: 1,
            car: 0,
            time,
            score: 0,
            dropped: false,
            top,
            avg: 0.0,
            jump,
            slide,
            drift_best,
        };
        st.add_records("Alpha", 0, &me(100.0, 0.0, 0.0, 0.0, 0));
        assert!(st.rec("Alpha", false).is_none(), "nothing to keep: no record made");
        st.add_records("Alpha", 0, &me(100.0, 42.1257, 1.5, 12.25, 5300));
        st.add_records("Alpha", 0, &me(100.0, 40.0, 2.25, 0.0, 9000));
        st.add_records("Alpha", 0, &me(100.0, 900.0, 0.0, f64::NAN, -5));
        st.add_records("Alpha", 0, &me(11.0, 0.0, 0.0, 0.0, 0));
        for time in [12.75, 11.5, 13.0] {
            st.add_records("Alpha", 2, &me(time, 0.0, 0.0, 0.0, 0));
        }
        let want = Records { top: 42.126, drift: 9000.0, slide: 12.25, jump: 2.25, drag: 11.5 };
        assert_eq!(st.rec("Alpha", false).unwrap().records, want);
        assert_eq!(hexes(&st.stat_field(Some("Alpha"), false))[0..8], [0, 0, 0, 42126, 9000, 12250, 2250, 11500]);
        assert_eq!(hexes(&st.stat_field(Some("Alpha"), true))[0..8], [0; 8], "the month's: none");
        st.stop();
        let text = std::fs::read_to_string(dir.path().join("stats.json")).unwrap();
        assert!(text.contains(r#""best":{},"rec":{"top":42.126,"drift":9000,"slide":12.25,"jump":2.25,"drag":11.5}}"#), "{text}");
        let mut b = Stats::new(opts());
        assert_eq!(b.rec("Alpha", false).unwrap().records, want);
        // a hand-edited file: a figure no car makes, or no number, is none
        std::fs::write(
            dir.path().join("stats.json"),
            r#"{"personas":[{"name":"Okay","seen":"2026-10","all":{},"rec":{"top":1e9,"jump":"x","slide":-3,"drag":12}}]}"#,
        )
        .unwrap();
        let mut c = Stats::new(opts());
        assert_eq!(c.rec("Okay", false).unwrap().records, Records { drag: 12.0, ..Default::default() });
    }

    // ONL-7: the results store: room for new players when full, ... best times capped (the store's parts; the
    // rankings built once for many asks: lobby.rs)
    #[test]
    fn onl7_store_full_best_capped() {
        let t = Rc::new(Cell::new(T0));
        let mut o = StatsOptions::new(None, clocks(t.clone()), quiet());
        o.max = 3;
        let mut st = Stats::new(o);
        st.add_race(&race(0, false, 1, &[("Old1", 1, 50.0), ("Old2", 2, 51.0)]));
        st.add_race(&race(0, false, 1, &[("New1", 1, 50.0), ("New2", 2, 51.0)]));
        assert!(st.len() <= 3, "{} records (the cap is 3)", st.len());
        assert!(st.rec("New1", false).is_some() && st.rec("New2", false).is_some(), "the newest players are recorded");
        let mut s2 = Stats::new(StatsOptions::new(None, clocks(t), quiet()));
        for tr in 0..400 {
            s2.add_race(&race(0, false, tr, &[("Alpha", 1, 50.0), ("Bravo", 2, 51.0)]));
        }
        let n = s2.rec("Alpha", false).unwrap().best.len();
        assert!(n <= 300, "{n} best times kept");
    }

    // a file nested far deeper than a thread's stack (JSON.parse takes it: V8's parser keeps its own stack, and so
    // does jsonparse.rs): read on a thread with the online host's stack, the persona in it kept, nothing else; the
    // same for buddies.json.  (Before: the parsed value was cloned and dropped by recursion, and the stack overflow
    // aborted the whole server at its start, at every start.)
    #[test]
    fn a_deeply_nested_file_is_read() {
        let dir = tempfile::tempdir().unwrap();
        let d = dir.path().to_path_buf();
        let n = 200_000;
        let deep = "[".repeat(n) + &"]".repeat(n);
        let text = format!(
            r#"{{"personas":[{{"name":"Alpha","seen":"2026-10","all":{{"0":{{"races":2,"rw":1}},"1":{deep}}},"x":{deep}}},{deep}]}}"#
        );
        std::fs::write(d.join("stats.json"), &text).unwrap();
        std::fs::write(
            d.join("buddies.json"),
            format!(r#"{{"personas":[{{"name":"Alpha","seen":"2026-10","B":["Bravo",{deep}]}},{deep}],"x":{deep}}}"#),
        )
        .unwrap();
        // the stats.json of a file that does not parse, deep: moved aside
        let bad = d.join("bad");
        std::fs::create_dir_all(&bad).unwrap();
        std::fs::write(bad.join("stats.json"), format!("{{\"personas\":[{deep},{}", "[".repeat(n))).unwrap();
        let t = std::thread::Builder::new()
            .stack_size(2 << 20)
            .spawn(move || {
                let st = Stats::new(StatsOptions::new(Some(d.clone()), clocks(Rc::new(Cell::new(T0))), quiet()));
                let alpha = st.p.get("alpha").map(|r| (r.all[0], r.all[1].is_some()));
                let b = crate::buddy::Buddies::new(Some(d.clone()), Default::default());
                let bad = Stats::new(StatsOptions::new(Some(bad), clocks(Rc::new(Cell::new(T0))), quiet()));
                (alpha, b.serialize(), bad.p.len())
            })
            .unwrap();
        let (alpha, buddies, bad) = t.join().expect("read on the online host's stack");
        // (the array in "1" is a count block of none of the fields, as `clean` makes it)
        assert_eq!(alpha, Some((Some(Counts { races: 2.0, rw: 1.0, ..Default::default() }), true)));
        assert_eq!(
            buddies,
            r#"{"v":3,"note":"NFSU2 online Messenger lists (persona names; friend requests and messages waiting for their persona)","personas":[{"name":"Alpha","seen":"2026-10","B":["Bravo"]}]}"#
        );
        assert_eq!(bad, 0);
    }

    #[test]
    fn node_base64_as_node_reads_it() {
        for (s, hex) in [
            ("QUJD=REVG", "414243"),
            ("QU JD", "414243"),
            ("QU*JD", "414243"),
            ("QQ", "41"),
            ("Q", ""),
            ("QUJ", "4142"),
            ("QU=JD", "41"),
            ("-_8", "fbff"),
            ("+/8", "fbff"),
            ("not base64 at all!", "9e8b5b6ac7bae1ab5a96"),
            ("undefined", "ba775e7e29de"),
            ("=QUJD", ""),
            ("QUJ=D", "4142"),
        ] {
            assert_eq!(jsx::hex(&node_base64(s)), hex, "{s}");
        }
    }
}
