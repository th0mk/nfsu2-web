//! The daily treasure hunt's board (docs/web/PLAYING.md; the page's web/src/app/treasure.ts and huntboard.ts): career
//! free roam hides 15 diamonds a UTC day, the same spots for everyone that day (runtime/treasure.c), and a page that
//! found all 15 reports how long its hunt took (active free-roam milliseconds from the 1st diamond to the 15th).  The
//! board is how many finished a day, and the 10 fastest by their claimed names.
//!
//! - `POST /hunt` `{"day":<UTC day: floor(ms since 1970 / 86400000)>,"ms":<the hunt's time>,"id":<the page's id
//!   (page-id.ts)>}`, with the keyphrase's token as /names takes it (`Authorization: Bearer ...`) when the page has
//!   one -> 200 `{"done":"new"|"had","ms":<the time kept>, and the day's board as GET has it}`.  A player is its
//!   keyphrase (nfsu2-claims `owner_of_token`), else its page id: one time a player a day, the first (a second hunt
//!   that day, on another browser or a second profile of the same keyphrase, knows where the diamonds are: "had").
//!   400 `{"error":"invalid"}` (no JSON object), "day" (neither today nor yesterday: yesterday for a hunt finished
//!   just before midnight and reported after it), "time" (under MIN_MS or over a day); 401 "id" (neither a token nor
//!   a page id); 507 "full" (MAX_A_DAY times that day already); 429 plain (per client, and twice that per site,
//!   REPORT_BURST reports that were not refused, then REPORT_RATE a second).
//! - `GET /hunt[?day=<UTC day>]` (today when none) -> `{"day":...,"finished":<players with a time that day>,"best":
//!   [{"name","ms"}, the TOP fastest, ties by who reported first]}`; a day not kept (KEEP_DAYS) is an empty board.
//!   400 "invalid" for a day that is no number; 429 plain (per client READ_BURST reads, then READ_RATE a second).
//!
//! The board names a player by its keyphrase's claimed name as of the read (a name changed since shows as the new
//! one; a claim let go, or a name the operator took away, shows none): a time without one (no keyphrase, or one that
//! claimed no name) is counted in "finished" but not listed, as free roam's friends are claimed names only.  A
//! script can make up page ids and tokens as it can for /seen (days.rs): the count is a rough one, rate-limited per
//! client; the names listed are claims, which take a keyphrase the cloud keeps and Cloudflare's check.
//!
//! MIN_MS: the diamonds are at least ~400 m apart (runtime/treasure.c), so 14 legs are 5.6 km at the least as the
//! crow flies, about 70 s at the game's top speed (~300 km/h) on a straight line with no stop; along the streets,
//! with turns, twice that is still quicker than any real hunt.  Below 120 s a time is a cheat or a broken clock.
//!
//! Kept in <state>/hunts.json (the last KEEP_DAYS days; each day a random key, under which a player is a keyed hash
//! of 64 bits, as days.rs counts browsers, so nothing links one day's players to the next day's; a keyphrase's
//! owner hash, as names.json has it, beside its time, to name it), written whole when it changed (every minute, and
//! at the stop).  The console has each time reported, by its claimed name when it has one.

use std::collections::{BTreeMap, HashSet};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use hmac::{Hmac, Mac};
use parking_lot::Mutex;
use serde_json::{Value, json};

use nfsu2_claims::{DAY_MS, Owner};
use nfsu2_headers::Cache;
use nfsu2_http::{Request, Response};
use nfsu2_util::{Rate, js, log};

use super::{App, Fail, Res, url};

/// the days kept: today and the six before it
pub const KEEP_DAYS: i64 = 7;
/// the fastest hunt believed (the module's doc)
pub const MIN_MS: i64 = 120_000;
/// the slowest: a day's diamonds are there for that day only
pub const MAX_MS: i64 = DAY_MS;
/// the times listed
pub const TOP: usize = 10;
/// a day holds this many times at most (some 100 bytes each in hunts.json)
pub const MAX_A_DAY: usize = 100_000;
pub const READ_BURST: f64 = 60.0;
pub const READ_RATE: f64 = 1.0;
/// a page reports once a day (one more for yesterday's, now and then); a few to spare for retries
pub const REPORT_BURST: f64 = 5.0;
pub const REPORT_RATE: f64 = 1.0 / 60.0;
/// the most of a report's body (a day, a time and a page id)
const MAX_BODY: usize = 1024;

/// the UTC day of `ms`, as the page counts them (achievements.ts utcDay)
pub fn utc_day(ms: i64) -> i64 {
    ms.div_euclid(DAY_MS)
}

/// one player's time: its keyed hash, the time, when it came, and its keyphrase's owner (none: a page id's)
#[derive(Clone, Debug)]
struct Run {
    h: u64,
    ms: i64,
    at: i64,
    owner: Option<Owner>,
}

/// a day's times, fastest first (ties: the one reported first)
struct Day {
    key: Vec<u8>,
    seen: HashSet<u64>,
    runs: Vec<Run>,
}

impl Day {
    fn new() -> Day {
        Day { key: nfsu2_util::rand::bytes(32), seen: HashSet::new(), runs: Vec::new() }
    }

    fn hash(&self, who: &str) -> u64 {
        let mut mac = <Hmac<sha2::Sha256> as Mac>::new_from_slice(&self.key).expect("HMAC takes any key");
        mac.update(who.as_bytes());
        u64::from_le_bytes(mac.finalize().into_bytes()[..8].try_into().expect("8 bytes"))
    }
}

/// what a report did
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Done {
    /// the player's time for the day, kept
    New,
    /// the player had one that day already: this one (its first)
    Had(i64),
    /// the day holds MAX_A_DAY
    Full,
}

/// the boards of the last KEEP_DAYS days, and the limits
pub struct Hunts {
    file: Option<PathBuf>,
    days: Mutex<BTreeMap<i64, Day>>,
    changed: AtomicBool,
    reads: Rate,
    reports: Rate,
}

impl Hunts {
    /// the days kept in the state folder's hunts.json (none when it is missing or unreadable)
    pub fn open(state: &str, now: i64) -> Hunts {
        let file = PathBuf::from(state).join("hunts.json");
        let days = std::fs::read_to_string(&file).ok().map(|s| from_json(&s)).unwrap_or_default();
        let h = Hunts { file: Some(file), days: Mutex::new(days), changed: AtomicBool::new(false), reads: rate(true), reports: rate(false) };
        h.prune(now);
        h
    }

    #[cfg(test)]
    pub fn in_memory() -> Hunts {
        Hunts { file: None, days: Mutex::default(), changed: AtomicBool::new(false), reads: rate(true), reports: rate(false) }
    }

    /// the days before KEEP_DAYS go, and the buckets that are full again (every minute)
    pub fn prune(&self, now: i64) {
        let first = utc_day(now) - (KEEP_DAYS - 1);
        let mut d = self.days.lock();
        let n = d.len();
        d.retain(|day, _| *day >= first);
        if d.len() != n {
            self.changed.store(true, Ordering::Relaxed);
        }
        drop(d);
        self.reads.prune();
        self.reports.prune();
    }

    /// `who` ("o:<owner>" or "p:<page id>") finished day `day`'s hunt in `ms` (checked by the caller)
    pub fn report(&self, day: i64, who: &str, owner: Option<Owner>, ms: i64, now: i64) -> Done {
        let mut days = self.days.lock();
        let d = days.entry(day).or_insert_with(Day::new);
        let h = d.hash(who);
        if d.seen.contains(&h) {
            return Done::Had(d.runs.iter().find(|r| r.h == h).map_or(0, |r| r.ms));
        }
        if d.runs.len() >= MAX_A_DAY {
            return Done::Full;
        }
        let at = d.runs.partition_point(|r| r.ms <= ms);
        d.runs.insert(at, Run { h, ms, at: now, owner });
        d.seen.insert(h);
        self.changed.store(true, Ordering::Relaxed);
        Done::New
    }

    /// day `day`: how many finished, and the TOP fastest that `name` names (an owner's name now; None: not listed)
    pub fn board(&self, day: i64, name: impl Fn(&str) -> Option<String>) -> (usize, Vec<(String, i64)>) {
        let days = self.days.lock();
        let Some(d) = days.get(&day) else { return (0, Vec::new()) };
        let best = d.runs.iter().filter_map(|r| Some((name(r.owner.as_deref()?)?, r.ms))).take(TOP).collect();
        (d.runs.len(), best)
    }

    /// hunts.json written when anything changed since; true when it was
    pub fn save(&self) -> Result<bool, String> {
        let Some(file) = &self.file else { return Ok(false) };
        if !self.changed.swap(false, Ordering::Relaxed) {
            return Ok(false);
        }
        let body = to_json(&self.days.lock());
        nfsu2_claims::write_atomic(file, body.as_bytes()).inspect_err(|_| self.changed.store(true, Ordering::Relaxed))?;
        Ok(true)
    }
}

fn rate(reads: bool) -> Rate {
    if reads { Rate::new(READ_BURST, READ_RATE) } else { Rate::new(REPORT_BURST, REPORT_RATE) }
}

/// {"days":[{"day":20397,"key":<base64url>,"runs":[{"h":<16 hex digits>,"ms":...,"at":...,"o":<owner>}]}]}
fn to_json(days: &BTreeMap<i64, Day>) -> String {
    let days: Vec<Value> = days
        .iter()
        .map(|(day, d)| {
            let runs: Vec<Value> = d
                .runs
                .iter()
                .map(|r| {
                    let mut m = json!({ "h": format!("{:016x}", r.h), "ms": r.ms, "at": r.at });
                    if let Some(o) = &r.owner {
                        m["o"] = o.to_string().into();
                    }
                    m
                })
                .collect();
            json!({ "day": day, "key": js::base64url(&d.key), "runs": runs })
        })
        .collect();
    json!({ "days": days }).to_string()
}

fn from_json(s: &str) -> BTreeMap<i64, Day> {
    let mut out = BTreeMap::new();
    let Ok(v) = serde_json::from_str::<Value>(s) else { return out };
    for d in v.get("days").and_then(Value::as_array).into_iter().flatten() {
        let (Some(day), Some(key)) = (d.get("day").and_then(Value::as_i64), d.get("key").and_then(Value::as_str)) else { continue };
        let key = js::from_base64url(key);
        if key.len() != 32 {
            continue;
        }
        let mut runs: Vec<Run> = d
            .get("runs")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter_map(|r| {
                Some(Run {
                    h: u64::from_str_radix(r.get("h")?.as_str()?, 16).ok()?,
                    ms: r.get("ms")?.as_i64()?,
                    at: r.get("at")?.as_i64()?,
                    owner: r.get("o").and_then(Value::as_str).map(Arc::from),
                })
            })
            .take(MAX_A_DAY)
            .collect();
        runs.sort_by_key(|r| (r.ms, r.at));
        let seen = runs.iter().map(|r| r.h).collect();
        out.insert(day, Day { key, seen, runs });
    }
    out
}

/// "6:12.4": a time on the console
fn clock(ms: i64) -> String {
    format!("{}:{:02}.{}", ms / 60_000, ms / 1000 % 60, ms / 100 % 10)
}

impl App {
    /// an owner's claimed name as the board shows it: none for a name the operator took away (names.rs name_status)
    fn hunt_name(&self, owner: &str, now: i64) -> Option<String> {
        let n = self.names.as_ref()?.claims.mine(owner, now)?.name;
        (!self.bans.deny().has(&n)).then_some(n)
    }

    fn hunt_board(&self, day: i64, now: i64) -> Value {
        let (finished, best) = self.hunts.board(day, |o| self.hunt_name(o, now));
        let best: Vec<Value> = best.into_iter().map(|(name, ms)| json!({ "name": name, "ms": ms })).collect();
        json!({ "day": day, "finished": finished, "best": best })
    }

    /// /hunt (the treasure hunt's board): see this module's doc
    pub(super) async fn hunt_route(&self, q: &mut Request, s: &mut Response, u: &url::Url) -> Res {
        // only this server's own pages: a page of another site must not report a time through a player's browser
        let site_ok = q.headers.get("sec-fetch-site").is_none_or(|x| x == "same-origin");
        if !self.own_page(q) || !site_ok {
            return self.plain(q, s, 403, "Forbidden", &[], Cache::Store, true).await;
        }
        let (_, key, site) = self.client(q);
        let now = nfsu2_util::now_ms();
        let today = utc_day(now);
        match q.method.as_str() {
            "GET" | "HEAD" => {
                if !self.hunts.reads.take(&key, &site) {
                    return self.plain(q, s, 429, "Too many requests", &[("retry-after", "60")], Cache::Store, true).await;
                }
                let day = match u.param("day") {
                    None => today,
                    Some(d) => match d.parse::<i64>() {
                        Ok(d) => d,
                        Err(_) => return self.json(q, s, 400, &json!({ "error": "invalid" })).await,
                    },
                };
                let v = self.hunt_board(day, now);
                self.json(q, s, 200, &v).await
            }
            "POST" => {
                let body = match q.read_body(MAX_BODY).await {
                    Ok(b) => b,
                    Err(nfsu2_http::BodyError::TooLarge) => return self.json(q, s, 413, &json!({ "error": "large" })).await,
                    Err(nfsu2_http::BodyError::Broken) => {
                        s.destroy();
                        return Err(Fail::Gone);
                    }
                };
                let Ok(Value::Object(b)) = serde_json::from_slice::<Value>(&body) else {
                    return self.json(q, s, 400, &json!({ "error": "invalid" })).await;
                };
                let day = b.get("day").and_then(Value::as_i64);
                let Some(day) = day.filter(|d| *d == today || *d == today - 1) else {
                    return self.json(q, s, 400, &json!({ "error": "day" })).await;
                };
                let ms = b.get("ms").and_then(Value::as_f64).filter(|m| m.is_finite()).map(|m| m.round() as i64);
                let Some(ms) = ms.filter(|m| (MIN_MS..=MAX_MS).contains(m)) else {
                    return self.json(q, s, 400, &json!({ "error": "time" })).await;
                };
                let owner = super::names::token(q).and_then(nfsu2_claims::owner_of_token);
                let id = b.get("id").and_then(Value::as_str).filter(|i| super::days::id_ok(i));
                let who = match (&owner, id) {
                    (Some(o), _) => format!("o:{o}"),
                    (None, Some(i)) => format!("p:{i}"),
                    (None, None) => return self.json(q, s, 401, &json!({ "error": "id" })).await,
                };
                // (what is refused anyway costs nothing of the budget, as at /names)
                if !self.hunts.reports.take(&key, &site) {
                    return self.plain(q, s, 429, "Too many requests", &[("retry-after", "60")], Cache::Store, true).await;
                }
                let (done, kept) = match self.hunts.report(day, &who, owner.clone(), ms, now) {
                    Done::New => ("new", ms),
                    Done::Had(m) => ("had", m),
                    Done::Full => return self.json(q, s, 507, &json!({ "error": "full" })).await,
                };
                let mut v = self.hunt_board(day, now);
                if done == "new" {
                    let name = owner.as_deref().and_then(|o| self.hunt_name(o, now)).unwrap_or_else(|| "a player without a name".into());
                    let line = format!("[hunt] {name} found the day's diamonds in {} ({} so far)", clock(ms), v["finished"]);
                    log::limited("hunt", "times", &key, &line, 10);
                }
                v["done"] = done.into();
                v["ms"] = kept.into();
                self.json(q, s, 200, &v).await
            }
            _ => self.plain(q, s, 405, "Method not allowed", &[("allow", "GET, POST"), ("connection", "close")], Cache::Store, true).await,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::testkit::Kit;
    use super::*;

    const A: (&str, &str) = ("Authorization", "Bearer AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    const B: (&str, &str) = ("Authorization", "Bearer BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
    /// 2026-10-30T12:00Z
    const T0: i64 = 1_793_361_600_000;

    fn j(body: &[u8]) -> Value {
        serde_json::from_slice(body).unwrap()
    }

    #[test]
    fn one_time_a_player_a_day_the_first_fastest_first() {
        let h = Hunts::in_memory();
        let d = utc_day(T0);
        assert_eq!(h.report(d, "o:x", Some(Arc::from("x")), 400_000, T0), Done::New);
        assert_eq!(h.report(d, "o:x", Some(Arc::from("x")), 200_000, T0 + 1), Done::Had(400_000), "the first kept, not the best");
        assert_eq!(h.report(d, "o:y", Some(Arc::from("y")), 300_000, T0 + 2), Done::New);
        assert_eq!(h.report(d, "p:z", None, 250_000, T0 + 3), Done::New);
        assert_eq!(h.report(d, "o:w", Some(Arc::from("w")), 300_000, T0 + 4), Done::New);
        // the same player the next day is another
        assert_eq!(h.report(d + 1, "o:x", Some(Arc::from("x")), 200_000, T0 + DAY_MS), Done::New);
        // the page id's time counted, not listed; a tie goes to the first; an owner without a name not listed
        let name = |o: &str| (o != "w").then(|| o.to_uppercase());
        assert_eq!(h.board(d, name), (4, vec![("Y".into(), 300_000), ("X".into(), 400_000)]));
        assert_eq!(h.board(d, |o: &str| Some(o.to_string())).1[..2], [("y".into(), 300_000), ("w".into(), 300_000)]);
        assert_eq!(h.board(d - 1, name), (0, vec![]));
    }

    #[test]
    fn the_last_days_kept_through_a_restart() {
        let dir = tempfile::tempdir().unwrap();
        let state = dir.path().to_string_lossy().into_owned();
        let d = utc_day(T0);
        let h = Hunts::open(&state, T0);
        assert!(!h.save().unwrap(), "nothing yet: nothing written");
        h.report(d - 6, "o:old", Some(Arc::from("old")), 500_000, T0 - 6 * DAY_MS);
        h.report(d, "o:x", Some(Arc::from("x")), 400_000, T0);
        h.report(d, "p:AAAAAAAAAAAAAAAAAAAAAA", None, 300_000, T0);
        assert!(h.save().unwrap());
        let f = std::fs::read_to_string(dir.path().join("hunts.json")).unwrap();
        assert!(!f.contains("AAAAAAAAAAAAAAAAAAAAAA"), "no page id kept");
        drop(h);

        let h = Hunts::open(&state, T0 + 1000);
        assert_eq!(h.board(d, |o: &str| Some(o.to_string())), (2, vec![("x".into(), 400_000)]));
        assert_eq!(h.report(d, "o:x", Some(Arc::from("x")), 200_000, T0 + 1000), Done::Had(400_000), "still the player's");
        assert_eq!(h.report(d, "p:AAAAAAAAAAAAAAAAAAAAAA", None, 200_000, T0 + 1000), Done::Had(300_000));
        assert_eq!(h.board(d - 6, |o: &str| Some(o.to_string())).0, 1);
        // a day later the oldest is gone
        h.prune(T0 + DAY_MS);
        assert_eq!(h.board(d - 6, |o: &str| Some(o.to_string())).0, 0);
        assert_eq!(h.board(d, |o: &str| Some(o.to_string())).0, 2);
    }

    #[test]
    fn times_on_the_console() {
        assert_eq!(clock(372_400), "6:12.4");
        assert_eq!(clock(3_605_000), "60:05.0");
    }

    #[tokio::test]
    async fn reported_and_read_by_claimed_names() {
        let kit = Kit::start(&[], |_| {}).await;
        let today = utc_day(nfsu2_util::now_ms());
        let post = |who: Vec<(&'static str, &'static str)>, body: Value| {
            let kit = &kit;
            async move {
                let mut h = who;
                h.push(("Content-Type", "application/json"));
                let a = kit.request("POST", "/hunt", &h, body.to_string().as_bytes()).await;
                (a.status, serde_json::from_slice::<Value>(&a.body).unwrap_or(Value::Null))
            }
        };
        // A claims a name; B has a keyphrase and no name
        for k in [A, B] {
            assert_eq!(kit.request("PUT", "/vault", &[k, ("If-None-Match", "*")], b"saves").await.status, 201);
        }
        let claim = kit.request("POST", "/names", &[A, ("Content-Type", "application/json")], b"{\"name\":\"Alex\"}").await;
        assert_eq!(claim.status, 200);

        let (code, v) = post(vec![A], json!({ "day": today, "ms": 400_000.4 })).await;
        assert_eq!((code, &v["done"], &v["ms"], &v["finished"]), (200, &json!("new"), &json!(400_000), &json!(1)));
        assert_eq!(v["best"], json!([{ "name": "Alex", "ms": 400_000 }]));
        let (code, v) = post(vec![A], json!({ "day": today, "ms": 300_000 })).await;
        assert_eq!((code, &v["done"], &v["ms"]), (200, &json!("had"), &json!(400_000)));
        assert_eq!(post(vec![B], json!({ "day": today, "ms": 350_000 })).await.1["done"], "new");
        // the page id of a page without a keyphrase: counted, not listed
        let (code, v) = post(vec![], json!({ "day": today - 1, "ms": 350_000, "id": "AAAAAAAAAAAAAAAAAAAAAA" })).await;
        assert_eq!((code, &v["day"], &v["finished"], &v["best"]), (200, &json!(today - 1), &json!(1), &json!([])));

        let g = kit.get("/hunt", &[]).await;
        assert_eq!((g.status, g.header("cache-control")), (200, Some("private, no-store, no-transform")));
        assert_eq!(j(&g.body), json!({ "day": today, "finished": 2, "best": [{ "name": "Alex", "ms": 400_000 }] }));
        assert_eq!(j(&kit.get(&format!("/hunt?day={}", today - 3), &[]).await.body), json!({ "day": today - 3, "finished": 0, "best": [] }));
        assert_eq!(kit.get("/hunt?day=x", &[]).await.status, 400);
        // a name the operator takes away is not shown
        kit.app.bans.rename("Alex", &["Alex".to_string()], &Default::default(), "", nfsu2_util::now_ms()).unwrap();
        assert_eq!(j(&kit.get("/hunt", &[]).await.body)["best"], json!([]));
    }

    #[tokio::test]
    async fn implausible_reports_refused() {
        let kit = Kit::start(&[], |_| {}).await;
        let today = utc_day(nfsu2_util::now_ms());
        let post = |h: &'static [(&'static str, &'static str)], body: String| {
            let kit = &kit;
            async move { kit.request("POST", "/hunt", h, body.as_bytes()).await }
        };
        let id = "BBBBBBBBBBBBBBBBBBBBBB";
        for (body, code, e) in [
            (json!({ "day": today + 1, "ms": 300_000, "id": id }), 400, "day"),
            (json!({ "day": today - 2, "ms": 300_000, "id": id }), 400, "day"),
            (json!({ "day": today, "ms": MIN_MS - 1, "id": id }), 400, "time"),
            (json!({ "day": today, "ms": MAX_MS + 1, "id": id }), 400, "time"),
            (json!({ "day": today, "ms": "300000", "id": id }), 400, "time"),
            (json!({ "day": today, "ms": 300_000, "id": "bad id!" }), 401, "id"),
            (json!({ "day": today, "ms": 300_000 }), 401, "id"),
        ] {
            let a = post(&[], body.to_string()).await;
            assert_eq!((a.status, j(&a.body)), (code, json!({ "error": e })), "{body}");
        }
        assert_eq!(post(&[], "[1]".into()).await.status, 400);
        // another site's page reports nothing; other methods none
        let x = post(&[("Origin", "https://evil.example")], json!({ "day": today, "ms": 300_000, "id": id }).to_string()).await;
        assert_eq!(x.status, 403);
        assert_eq!(kit.request("PUT", "/hunt", &[], b"").await.status, 405);
        // the refusals cost nothing of the budget: REPORT_BURST times from one client, then 429
        assert_eq!(j(&kit.get("/hunt", &[]).await.body)["finished"], 0);
        for i in 0..REPORT_BURST as usize {
            let a = post(&[], json!({ "day": today, "ms": 300_000, "id": format!("{id}{i}") }).to_string()).await;
            assert_eq!(a.status, 200);
        }
        assert_eq!(post(&[], json!({ "day": today, "ms": 300_000, "id": id }).to_string()).await.status, 429);
        assert_eq!(j(&kit.get("/hunt", &[]).await.body)["finished"], REPORT_BURST as usize);
    }
}
