//! The races' results as the players' games showed them (RACERES, roam_core.h rc_raceres), for a race won by
//! cheating: a teleport to the finish, a speed hack, a race the game ended at once.  Every racer's game sends its
//! results once back in free roam, each row by the racer's player id: finished or not, its time, how far it came
//! (laps), the way it drove (m).  The hub takes a report only from a player of the race's lobby (the endpoint that
//! joined it with that id), naming its players, each as that endpoint (hub.rs `Joined`), one a lobby; it keeps a
//! race's reports until every racer's came, or RESULTS_WAIT_MS after the first, then judges it once.
//!
//! - the winner: the one who came first in more than half of the reports (one report cannot name a winner the others
//!   do not), at the slowest time any report gave it (one report's time cannot make it quicker).  A report's first is
//!   its quickest finisher; with none (none came the race's whole way), the game's own first row when every other one
//!   ended with the race, GAME_END_S or more after it: the game finished it without the way driven, as a cheat that
//!   ends its race at once does (a test's racewin, 2026-10-28: first in 4.76 s after 0.6 % of the way, the other
//!   racer 30 s later at 17 %)
//! - the race's length: what another finisher drove in it, by its own game (its own row), the shortest of them; never
//!   the winner's own report.  A racer still on the way when the race ended (30 s after the first finished) says
//!   nothing of it: scaled up from how far they came, two such racers made a sprint 8.8 km, and its honest winner
//!   (82.7 s, the second quickest of 176 wins of that sprint that morning) 381 km/h on average (nfsu2.xyz, 2026-10-29)
//! - a `[cheat]` line when the winner averaged RACE_MPS (350 km/h) or more over that length, which no car of the game
//!   does (the fastest tuned cars make 350 at the most, a race's average far less: a 2-lap circuit's was 112 km/h);
//!   when it came under DROVE_MIN of the way by every report, two at least (the game ended its race: one report alone
//!   is no more than a race whose other racers quit, nfsu2.xyz, 2026-10-30: a drag whose host quit had the other racer
//!   "win" at 14.6 % of the way, by its game alone, which said it did not finish); when it won in under
//!   QUICK_SHARE of the race's quickest clean win the hub saw (`Results::best`: by the reports of two clients at
//!   least, with another finisher, no line; once the race had QUICK_WINS of them, by two winners at least), which a
//!   teleport to the finish with nobody else over the line does; or, once the race had
//!   QUICK_WINS clean wins, in under THIRD_SHARE of the third quickest other winner's (a car made faster than any
//!   tuning makes it: nfsu2.xyz, 2026-10-30, one player won a sprint in 51.9 s and a circuit in 83.0 s, the third
//!   quickest of 32 and 30 wins before 89.9 s and 152.1 s, and the next of the day's 548 and 423 wins 71.9 s and
//!   122.5 s; of the day's 4,727 wins with a third quickest to go by, the quickest other one took 0.78 of it), unless
//!   another finisher came close (the winner over THIRD_SHARE of its time: that time can be driven; nfsu2.xyz,
//!   2026-10-31, Online mode: four racers within 4 s, all under 70 % of a track's third quickest winner).  A next
//!   finisher far behind says nothing: the race ends 30 s after the first finished, so only a report the game did not
//!   make can show one.
//!
//! - a racer's car that jumped JUMP_M or more at once (a frame's move unlike the frame before's, not right after a
//!   stall of its states: racenet.c), as JUMP_SEEN of the other racers' games had it, or the only other one: a `[cheat]`
//!   line of its own, winner or not (nfsu2.xyz, 2026-11-04: a player said another stood still for laps, then finished
//!   at once; the reporter's game had that car jump 279.9 m once in that heat, where the race's other jumps were 44 m
//!   at the most, and the wait for its states 424 ms at the longest)
//!
//! The time rules leave out a winner whose next finisher came close (over THIRD_SHARE of its time: two cars cannot both
//! have cheated that much, the race's numbers to go by were off: nfsu2.xyz, 2026-11-02, about 30 lines of a race's
//! quickest clean win of 7:08 over races won in 2:24-2:53 with the next car 1-14 % behind), and one who drove under
//! DROVE_MIN of the way (its game ended the race when the others quit: 2-car drags "won" at 3.5 % of the way, one report
//! alone; the game's end of a race needs two reports).
//!
//! A URL race of heats sends a report a heat (RaceRes heat): each heat is judged as a race of its own, its clean wins
//! kept apart (another track each), its lines naming the heat.
//!
//! A drift race is won by points and is left out of the time rules.  The lines are for the operator (`nfsu2-server ctl kick|ban`), and
//! say how many of the race's reports saw it, and what the winner's own game said when it did not agree or sent no
//! report (nfsu2.xyz, 2026-10-31: three of five games had a stock car win a 19.2 km circuit in 49 s, its own game said
//! it did not finish).
//!
//! Every race judged is a `[race]` line of its result (`result_line`): its racers in order as the reports had them,
//! and the ones whose own game sent no report.  A racer's game that sends none has no `[race]` line of its own (its car
//! report goes with its results, roam_game.c) and is named by no `[cheat]` line unless it won by cheating: nfsu2.xyz,
//! 2026-10-31, a player said another "always wins races" in a Miata at 400 km/h, and nothing in the day's log named
//! that player in any race.

use std::collections::HashMap;
use std::sync::Arc;

// (the limits the Online mode's race checks share: RACE_MPS, THIRD_SHARE, QUICK_WINS, JUMP_M, MODE_DRIFT)
use nfsu2_anticheat::{JUMP_M, MODE_DRIFT, QUICK_WINS, Quickest, RACE_MPS, THIRD_SHARE, jump_seen};

use crate::wire::{LOBBY_PLAYERS, RS_FINISHED, RS_OURS, RaceRes, ResRow};

/// a race's reports are judged once every racer's came, or this long after the first (ms): every game sends its
/// report back in free roam, the host's 4 s after the others' (mprace.c)
pub(crate) const RESULTS_WAIT_MS: f64 = 90_000.0;
/// the races kept at once (a player's reports are one a lobby, none sooner than 20 s after its last: hub.rs)
const RACES_KEPT: usize = 4096;
/// a winner who came less than this share of the way by every report finished without driving it
const DROVE_MIN: f64 = 0.5;
/// a winner in under this share of the race's quickest clean win (once the race had QUICK_WINS clean wins, by two
/// winners at least); the clean wins kept (a race and its laps each), and the clients whose reports had a clean win's
/// winner first, at least (one client alone learns no clean win, so it cannot fill BEST_KEPT either)
const QUICK_SHARE: f64 = 0.5;
const BEST_KEPT: usize = 4096;
const BEST_SEEN: usize = 2;
/// a race ends 30 s after its first finished (mprace.c): the others' rows are that much later, this much at least (s)
const GAME_END_S: f64 = 25.0;
/// a winner who drove less than this share of the way: the line says how much it drove
const DROVE_SHORT: f64 = 0.9;

/// one race's reports so far: since when, each reporter's, each one's client (the reports' order: `Results::add`), and
/// its racers as the hub knew them then (their address and name, for the line: they may be gone by the time the race
/// is judged)
struct Race {
    at: f64,
    reports: Vec<(u32, RaceRes)>,
    clients: Vec<Arc<str>>,
    names: HashMap<u32, String>,
}

/// What a race's reports showed of its winner
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Verdict {
    pub lobby: u32,
    pub race: u32,
    pub mode: u8,
    pub laps: u8,
    /// its heat (1 ..) of heats, 0 and 0 for a race of one heat
    pub heat: u8,
    pub heats: u8,
    /// the winner's player id and time (s)
    pub winner: u32,
    pub time: f64,
    /// the race's length by another finisher's game (m), and the winner's average over it (m/s)
    pub length: Option<f64>,
    pub avg: Option<f64>,
    /// the next finisher (its id and time), if any
    pub next: Option<(u32, f64)>,
    /// the share of the way the winner drove, the most any report gave it, when that is under DROVE_SHORT
    pub drove: Option<f64>,
    /// the race's quickest clean win (s), when the winner took under QUICK_SHARE of it
    pub quickest: Option<f64>,
    /// the third quickest other winner's time (s) and the race's clean wins, when the winner took under THIRD_SHARE of
    /// it
    pub third: Option<(f64, u32)>,
    /// what the winner's own game's report said of it; None when no report of it came
    pub own: Option<OwnGame>,
    /// the reports that had it first, and all of them
    pub seen: usize,
    pub reports: usize,
    /// its racers as the hub knew them when their reports came (`Results::add`)
    pub names: HashMap<u32, String>,
}

/// What a race's winner's own game said of it: first (as `winner` reads a report) at that time, finished but not first,
/// or neither
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum OwnGame {
    First(f64),
    Finished(f64),
    Out,
}

/// A judged race's result (`result_line`): its racers in order, each with its time when it finished (the finishers by
/// time, each at the middle one of the times the reports gave it; then the others, as the reports named them), the
/// ones whose own game sent no report, how many reports came of how many racers, and its racers as the hub knew them
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Outcome {
    pub lobby: u32,
    pub race: u32,
    pub mode: u8,
    pub laps: u8,
    pub heat: u8,
    pub heats: u8,
    pub order: Vec<(u32, Option<f64>)>,
    pub silent: Vec<u32>,
    pub reports: usize,
    pub racers: usize,
    pub names: HashMap<u32, String>,
    /// the racers whose car jumped (`Jump`), each a `[cheat]` line (`jump_line`)
    pub jumps: Vec<Jump>,
}

/// A racer whose car jumped JUMP_M or more at once by the other racers' games: the largest jump each of them had
/// (theirs of JUMP_M or more, the largest first), how many other reports came, and where it came in the race (its place,
/// 1 ..; None: it did not finish)
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Jump {
    pub id: u32,
    pub seen: Vec<f64>,
    pub others: usize,
    pub place: Option<usize>,
}

/// The races' reports, until each is judged; the clean wins of each race (its name hash and laps) so far
#[derive(Default)]
pub(crate) struct Results {
    races: HashMap<(u32, u8), Race>,
    best: HashMap<(u32, u8, u8), Best>,
}

/// A race's clean wins: how many and the quickest winners (each once, by name, quickest first: `win`), and the
/// quickest of them, which the Online mode does not keep
#[derive(Clone, Debug, Default)]
struct Best {
    win: Quickest,
    quickest: f64,
}

impl Best {
    /// a clean win: it counts, its winner's time is one of the quickest
    fn add(&mut self, who: String, time: f64) {
        self.quickest = if self.win.wins == 0 { time } else { self.quickest.min(time) };
        self.win.wins += 1;
        self.win.add(who, time);
    }

    /// the quickest clean win, once there were QUICK_WINS of two winners at least (the 2026-11-05 security audit: from
    /// the first, a slow "race" of a race no one had won yet, 33:20, made every honest winner in under half of it with
    /// no car close behind a line, and a win with a line is never a clean win that brings it down)
    fn quick(&self) -> Option<f64> {
        (self.win.wins >= QUICK_WINS && self.win.top().len() >= 2).then_some(self.quickest)
    }
}

/// a racer as the clean wins know it: its name (lower case) from `names` (`10.77.3.41 "Bob"`), else its id
fn racer(names: &HashMap<u32, String>, id: u32) -> String {
    match names.get(&id).and_then(|w| w.split_once(" \"")) {
        Some((_, n)) => n.trim_end_matches('"').to_lowercase(),
        None => format!("{id:08x}"),
    }
}

/// a race's mode as a word (roam_core.h rc_racerep)
pub(crate) fn mode_name(mode: u8) -> &'static str {
    const MODES: [&str; 5] = ["circuit", "sprint", "drag", "drift", "URL"];
    MODES.get(mode as usize).copied().unwrap_or(if mode == 10 { "Street X" } else { "race" })
}

/// a time as m:ss.s
fn clock(s: f64) -> String {
    format!("{}:{:04.1}", (s / 60.0).floor(), s % 60.0)
}

/// a race's kind as its lines say it: "sprint, 2 laps", "URL, 3 laps, heat 2 of 3"
fn kind(mode: u8, laps: u8, heat: u8, heats: u8) -> String {
    let mut s = format!("{}, {} lap{}", mode_name(mode), laps, if laps == 1 { "" } else { "s" });
    if heats > 0 {
        s += &format!(", heat {heat} of {heats}");
    }
    s
}

/// A race's result line (`Outcome`): "[race] lobby 7dcff53a, race c7483590 (drag, 1 lap): the result by 3 of 4 racers'
/// games: 1 Ann 0:10.8, 2 Bob 0:27.0, DNF Cid; no report from its own game: 10.77.3.41 \"Ann\""; `who` as for `line`.
/// A drift race is won by points: its finishers in no order.
pub(crate) fn result_line(o: &Outcome, who: &dyn Fn(u32) -> String) -> String {
    let name = |id: u32| {
        let w = who(id);
        w.split_once(" \"").map_or(w.clone(), |(_, n)| n.trim_end_matches('"').to_string())
    };
    let drift = o.mode == MODE_DRIFT;
    let mut place = 0;
    let rows: Vec<String> = o
        .order
        .iter()
        .map(|&(id, t)| match (t, drift) {
            (Some(t), false) => {
                place += 1;
                format!("{place} {} {}", name(id), clock(t))
            }
            (Some(_), true) => format!("finished {}", name(id)),
            (None, _) => format!("DNF {}", name(id)),
        })
        .collect();
    let mut s = format!(
        "[race] lobby {:08x}, race {:08x} ({}): the result by {} of {} racers' games{}: {}",
        o.lobby,
        o.race,
        kind(o.mode, o.laps, o.heat, o.heats),
        o.reports,
        o.racers,
        if drift { " (won by points: no order)" } else { "" },
        rows.join(", ")
    );
    if !o.silent.is_empty() {
        s += &format!("; no report from its own game: {}", o.silent.iter().map(|&id| who(id)).collect::<Vec<_>>().join(", "));
    }
    s
}

/// One race's result by its reports (`Outcome`, its names left to the caller)
fn outcome(race: &Race) -> Option<Outcome> {
    let first = race.reports.first()?.1;
    let mut ids: Vec<u32> = Vec::new();
    for (_, r) in &race.reports {
        for x in &r.rows[..r.n] {
            if x.id != 0 && !ids.contains(&x.id) {
                ids.push(x.id);
            }
        }
    }
    let time = |id: u32| {
        let mut t: Vec<f64> = race
            .reports
            .iter()
            .flat_map(|(_, r)| r.rows[..r.n].iter().filter(|x| x.id == id && x.flags & RS_FINISHED != 0 && x.time > 0.0).map(|x| x.time))
            .collect();
        t.sort_by(f64::total_cmp);
        t.get(t.len().saturating_sub(1) / 2).copied()
    };
    let mut order: Vec<(u32, Option<f64>)> = ids.iter().map(|&id| (id, time(id))).collect();
    // (a stable sort: the ones who did not finish stay in the reports' order, after the finishers)
    order.sort_by(|a, b| match (a.1, b.1) {
        (Some(x), Some(y)) => x.total_cmp(&y),
        (Some(_), None) => std::cmp::Ordering::Less,
        (None, Some(_)) => std::cmp::Ordering::Greater,
        (None, None) => std::cmp::Ordering::Equal,
    });
    let silent = ids.iter().copied().filter(|id| !race.reports.iter().any(|(by, _)| by == id)).collect();
    let jumps = jumped(race, &order);
    Some(Outcome {
        lobby: first.lobby,
        race: first.race,
        mode: first.mode,
        laps: first.laps,
        heat: first.heat,
        heats: first.heats,
        order,
        silent,
        reports: race.reports.len(),
        racers: race.reports.iter().map(|(_, r)| r.n).max().unwrap_or(0),
        names: HashMap::new(),
        jumps,
    })
}

/// The racers whose car jumped JUMP_M or more at once by JUMP_SEEN of the other reports, or by the only other one
/// (`order`: the race's result, for their places)
fn jumped(race: &Race, order: &[(u32, Option<f64>)]) -> Vec<Jump> {
    let mut out = Vec::new();
    for (k, &(id, t)) in order.iter().enumerate() {
        let others: Vec<&RaceRes> = race.reports.iter().filter(|(by, _)| *by != id).map(|(_, r)| r).collect();
        let mut seen: Vec<f64> =
            others.iter().filter_map(|r| r.rows[..r.n].iter().find(|x| x.id == id)).map(|x| x.jump).filter(|&j| j >= JUMP_M).collect();
        seen.sort_by(|a, b| b.total_cmp(a));
        if jump_seen(seen.len(), others.len() == 1) {
            out.push(Jump { id, seen, others: others.len(), place: t.map(|_| k + 1) });
        }
    }
    out
}

/// A jump's `[cheat]` line (`Jump`); `who` as for `line`
pub(crate) fn jump_line(o: &Outcome, j: &Jump, who: &dyn Fn(u32) -> String) -> String {
    let each: Vec<String> = j.seen.iter().map(|m| format!("{m:.0}")).collect();
    format!(
        "[cheat] {}: its car jumped {:.0} m at once in the race of lobby {:08x} (race {:08x}: {}), as {} of {} other players' \
         games had it (the largest each: {} m; honest races' largest are under 50 m); {}",
        who(j.id),
        j.seen.first().copied().unwrap_or(0.0),
        o.lobby,
        o.race,
        kind(o.mode, o.laps, o.heat, o.heats),
        j.seen.len(),
        j.others,
        each.join(", "),
        match j.place {
            Some(1) if o.mode != MODE_DRIFT => "it won".to_string(),
            Some(p) if o.mode != MODE_DRIFT => format!("it came {p}"),
            Some(_) => "it finished".to_string(),
            None => "it did not finish".to_string(),
        }
    )
}

/// A verdict's `[cheat]` line; `who`: a player's address and name by its id (`10.77.3.41 "Bob"`)
pub(crate) fn line(v: &Verdict, who: &dyn Fn(u32) -> String) -> String {
    let mut s = format!(
        "[cheat] {}: won the race of lobby {:08x} (race {:08x}: {}) in {}",
        who(v.winner),
        v.lobby,
        v.race,
        kind(v.mode, v.laps, v.heat, v.heats),
        clock(v.time)
    );
    if let (Some(l), Some(a)) = (v.length, v.avg) {
        s += &format!(
            ", {:.0} km/h on average over the race's {:.1} km (as another finisher drove it){}",
            a * 3.6,
            l / 1000.0,
            if a >= RACE_MPS { ", which no car does" } else { "" }
        );
    }
    if let Some(q) = v.quickest {
        s += &format!(", under {:.0} % of the race's quickest clean win ({})", QUICK_SHARE * 100.0, clock(q));
    }
    if let Some((t, wins)) = v.third {
        s += &format!(
            ", under {:.0} % of the third quickest other winner's time of the race's {wins} clean wins ({})",
            THIRD_SHARE * 100.0,
            clock(t)
        );
    }
    if let Some(d) = v.drove {
        s += &format!(", having driven {:.1} % of the way", d * 100.0);
    }
    match v.next {
        Some((id, t)) => s += &format!("; the next finisher's {}, {}", clock(t), who(id)),
        None => s += "; nobody else finished (the race ends 30 s after the first)",
    }
    s += &format!(": as {} of {} players' games saw it", v.seen, v.reports);
    match v.own {
        None => s += "; its own game sent no report",
        Some(OwnGame::First(t)) if (t - v.time).abs() >= 0.5 => s += &format!("; its own game: first in {}", clock(t)),
        Some(OwnGame::First(_)) => {}
        Some(OwnGame::Finished(t)) => s += &format!("; its own game: not first, finished in {}", clock(t)),
        Some(OwnGame::Out) => s += "; its own game: did not finish",
    }
    s
}

/// a report's winner: its quickest finisher, or (none finished) the game's first row when every other one ended with
/// the race, GAME_END_S or more after it
fn winner(r: &RaceRes) -> Option<ResRow> {
    let rows = &r.rows[..r.n];
    let quickest = rows.iter().filter(|w| w.flags & RS_FINISHED != 0 && w.time > 0.0).min_by(|a, b| a.time.total_cmp(&b.time));
    if let Some(w) = quickest {
        return Some(*w);
    }
    let (first, rest) = rows.split_first()?;
    (first.time > 0.0 && !rest.is_empty() && rest.iter().all(|x| x.time >= first.time + GAME_END_S)).then_some(*first)
}

/// a report's whole way (laps): as far as its finishers came (whole laps), or with none the race's laps
fn whole_way(r: &RaceRes) -> f64 {
    let far = r.rows[..r.n].iter().filter(|x| x.flags & RS_FINISHED != 0).map(|x| x.done.floor()).fold(0.0, f64::max);
    if far >= 1.0 { far } else { (r.laps as f64).max(1.0) }
}

impl Results {
    /// a player's report of a race (one a reporter: a second one of it counts for nothing), from its game's `client`
    /// (an opaque id of its address: hub.rs `frame_of`), and `who` its racers by id as the hub knows them now; false
    /// when there is no room for a race more
    pub(crate) fn add(&mut self, now: f64, reporter: u32, client: Arc<str>, r: RaceRes, who: &dyn Fn(u32) -> Option<String>) -> bool {
        let key = (r.lobby, r.heat);
        if !self.races.contains_key(&key) && self.races.len() >= RACES_KEPT {
            return false;
        }
        let race =
            self.races.entry(key).or_insert_with(|| Race { at: now, reports: Vec::new(), clients: Vec::new(), names: HashMap::new() });
        if !race.reports.iter().any(|(id, _)| *id == reporter) && race.reports.len() < LOBBY_PLAYERS {
            race.reports.push((reporter, r));
            race.clients.push(client);
            for x in &r.rows[..r.n] {
                if x.id != 0
                    && !race.names.contains_key(&x.id)
                    && let Some(w) = who(x.id)
                {
                    race.names.insert(x.id, w);
                }
            }
        }
        true
    }

    /// the cheats' verdicts of `judged`
    #[cfg(test)]
    pub(crate) fn due(&mut self, now: f64) -> Vec<Verdict> {
        self.judged(now).into_iter().filter_map(|(_, v)| v).collect()
    }

    /// the races due (every racer's report came, or RESULTS_WAIT_MS went by), judged and forgotten: each one's result,
    /// and a cheat's verdict when its winner cheated.  A clean win by the reports of two clients or more, with another
    /// finisher, may be its race's quickest (the 2026-11-05 security audit: by two reports, two endpoints of one
    /// browser made a race's clean win of their own).
    pub(crate) fn judged(&mut self, now: f64) -> Vec<(Outcome, Option<Verdict>)> {
        let due: Vec<(u32, u8)> = self
            .races
            .iter()
            .filter(|(_, r)| {
                let racers = r.reports.iter().map(|(_, x)| x.n).max().unwrap_or(0);
                now - r.at >= RESULTS_WAIT_MS || r.reports.len() >= racers
            })
            .map(|(&k, _)| k)
            .collect();
        let mut out = Vec::new();
        for l in due {
            let Some(race) = self.races.remove(&l) else { continue };
            let Some(o) = outcome(&race) else { continue };
            let mut cheat = None;
            if let Some((v, is)) = judge(&race.reports, &self.best, &race.names) {
                let k = (v.race, v.laps, v.heat);
                if is {
                    cheat = Some(Verdict { names: race.names.clone(), ..v });
                } else if v.next.is_some()
                    && clients_first(&race, v.winner) >= BEST_SEEN
                    && (self.best.len() < BEST_KEPT || self.best.contains_key(&k))
                {
                    self.best.entry(k).or_default().add(racer(&race.names, v.winner), v.time);
                }
            }
            out.push((Outcome { names: race.names, ..o }, cheat));
        }
        out
    }
}

/// the clients whose reports of a race had `w` first (each once)
fn clients_first(race: &Race, w: u32) -> usize {
    let mut seen: Vec<&Arc<str>> = Vec::new();
    for ((_, r), c) in race.reports.iter().zip(&race.clients) {
        if winner(r).is_some_and(|x| x.id == w) && !seen.contains(&c) {
            seen.push(c);
        }
    }
    seen.len()
}

/// One race's reports judged (`best`: the races' clean wins; `names`: its racers, `Race::names`): its winner's
/// verdict, and whether it is a cheat's
fn judge(reports: &[(u32, RaceRes)], best: &HashMap<(u32, u8, u8), Best>, names: &HashMap<u32, String>) -> Option<(Verdict, bool)> {
    let first = reports.first()?.1;
    if first.mode == MODE_DRIFT {
        return None;
    }
    // the winner, in more than half of the reports
    let winners: Vec<ResRow> = reports.iter().filter_map(|(_, r)| winner(r)).collect();
    let w = winners.iter().map(|x| x.id).find(|&id| id != 0 && winners.iter().filter(|x| x.id == id).count() * 2 > reports.len())?;
    let seen = winners.iter().filter(|x| x.id == w).count();
    // its time: the slowest any report that had it first gave it; the most of the way any report says it drove
    let time = winners.iter().filter(|x| x.id == w).map(|x| x.time).fold(0.0, f64::max);
    let drove = reports
        .iter()
        .flat_map(|(_, r)| r.rows[..r.n].iter().filter(|x| x.id == w).map(|x| (x.done / whole_way(r)).min(1.0)))
        .fold(0.0, f64::max);
    // the race's length: each other finisher's own way, the shortest
    let length = reports
        .iter()
        .filter(|(by, _)| *by != w)
        .filter_map(|(by, r)| {
            let own = r.rows[..r.n].iter().find(|x| x.flags & RS_OURS != 0 && x.id == *by)?;
            (own.flags & RS_FINISHED != 0 && own.dist > 0.0).then_some(own.dist)
        })
        .reduce(f64::min);
    let avg = length.map(|l| l / time);
    // the next finisher: the quickest other one any report has
    let next = reports
        .iter()
        .flat_map(|(_, r)| r.rows[..r.n].iter().filter(|x| x.id != w && x.flags & RS_FINISHED != 0 && x.time > 0.0).map(|x| (x.id, x.time)))
        .min_by(|a, b| a.1.total_cmp(&b.1));
    // the winner's own game: its report's winner, else its row there
    let own = reports.iter().find(|(by, _)| *by == w).map(|(_, r)| match (winner(r), r.rows[..r.n].iter().find(|x| x.id == w)) {
        (Some(x), _) if x.id == w => OwnGame::First(x.time),
        (_, Some(x)) if x.flags & RS_FINISHED != 0 && x.time > 0.0 => OwnGame::Finished(x.time),
        _ => OwnGame::Out,
    });
    let b = best.get(&(first.race, first.laps, first.heat));
    // (the time rules: none for a winner with another car close behind, nor for one its game ended the race of)
    let close = next.is_some_and(|(_, t)| time >= THIRD_SHARE * t);
    let timed = !close && drove >= DROVE_MIN;
    let quickest = b.and_then(Best::quick).filter(|&q| timed && time < QUICK_SHARE * q);
    let third = b.and_then(|b| Some((b.win.third(&racer(names, w))?, b.win.wins))).filter(|&(t, _)| timed && time < THIRD_SHARE * t);
    let ended = drove < DROVE_MIN && seen >= 2;
    let cheat = avg.is_some_and(|a| !close && a >= RACE_MPS) || ended || quickest.is_some() || third.is_some();
    let v = Verdict {
        lobby: first.lobby,
        race: first.race,
        mode: first.mode,
        laps: first.laps,
        heat: first.heat,
        heats: first.heats,
        winner: w,
        time,
        length,
        avg,
        next,
        drove: (drove < DROVE_SHORT).then_some(drove),
        quickest,
        third,
        own,
        seen,
        reports: reports.len(),
        names: HashMap::new(),
    };
    Some((v, cheat))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// a report of lobby `lobby` (a 2-lap sprint) by `by`: rows (id, finished, time, laps, m)
    fn rep_of(lobby: u32, rows: &[(u32, bool, f64, f64, f64)], by: u32) -> RaceRes {
        let mut r = RaceRes {
            lobby,
            race: 0x0BAD_F00D,
            mode: 1,
            laps: 2,
            heat: 0,
            heats: 0,
            n: rows.len(),
            rows: [ResRow::default(); LOBBY_PLAYERS],
        };
        for (k, &(id, fin, time, done, dist)) in rows.iter().enumerate() {
            let flags = if fin { RS_FINISHED } else { 0 } | if id == by { RS_OURS } else { 0 };
            r.rows[k] = ResRow { id, flags, time, done, dist, jump: 0.0 };
        }
        r
    }

    fn rep(rows: &[(u32, bool, f64, f64, f64)], by: u32) -> RaceRes {
        rep_of(0xA1, rows, by)
    }

    /// reporter `by`'s client: a client of its own
    fn client(by: u32) -> Arc<str> {
        format!("client {by}").into()
    }

    /// the cheats of the races of these reports, each judged once all its reports came
    fn judged_in(x: &mut Results, reports: &[(u32, RaceRes)]) -> Option<Verdict> {
        for (by, r) in reports {
            x.add(0.0, *by, client(*by), *r, &|_| None);
        }
        let mut v = x.due(0.0);
        assert!(v.len() <= 1);
        v.pop()
    }

    fn judged(reports: &[(u32, RaceRes)]) -> Option<Verdict> {
        judged_in(&mut Results::default(), reports)
    }

    // every race judged is a result line: its racers in order, a finisher at the middle one of the reports' times, the
    // ones whose own game sent nothing named with their address (nfsu2.xyz, 2026-10-31: a player said another always
    // won, and its game sent no report of any race); a drift race's finishers in no order
    #[test]
    fn a_race_is_a_result_line() {
        let mut x = Results::default();
        let who = |id: u32| format!("10.77.0.{id} \"P{id}\"");
        let by = |r: &[(u32, bool, f64, f64, f64)], b: u32, lobby: u32| rep_of(lobby, r, b);
        let rows2 = [(1, true, 95.0, 2.0, 5800.0), (2, true, 101.0, 2.0, 5810.0), (3, false, 110.0, 1.5, 4300.0)];
        let rows3 = [(1, true, 95.4, 2.0, 5800.0), (2, true, 101.0, 2.0, 5810.0), (3, false, 110.0, 1.5, 4300.0)];
        x.add(0.0, 2, client(2), by(&rows2, 2, 0xA1), &|id| Some(who(id)));
        x.add(0.0, 3, client(3), by(&rows3, 3, 0xA1), &|id| Some(who(id)));
        assert_eq!(x.judged(80_000.0), vec![], "two of three reports: the third may come");
        let j = x.judged(RESULTS_WAIT_MS);
        assert_eq!(j.len(), 1);
        assert_eq!(j[0].1, None, "no cheat");
        assert_eq!(
            result_line(&j[0].0, &who),
            "[race] lobby 000000a1, race 0badf00d (sprint, 2 laps): the result by 2 of 3 racers' games: 1 P1 1:35.0, 2 P2 \
             1:41.0, DNF P3; no report from its own game: 10.77.0.1 \"P1\""
        );
        // a drift race
        let mut d = by(&rows2, 2, 0xA2);
        d.mode = MODE_DRIFT;
        x.add(0.0, 2, client(2), d, &|_| None);
        let j = x.judged(RESULTS_WAIT_MS);
        assert_eq!(
            result_line(&j[0].0, &|id| format!("p{id}")),
            "[race] lobby 000000a2, race 0badf00d (drift, 2 laps): the result by 1 of 3 racers' games (won by points: no \
             order): finished p1, finished p2, DNF p3; no report from its own game: p1, p3"
        );
    }

    // a teleport to the finish: Bob (2) in 21 s, Ann (1) 1:30 later; by her own game the sprint is 5.8 km
    #[test]
    fn a_win_no_car_makes_is_a_cheat() {
        let rows = [(2, true, 21.0, 2.0, 400.0), (1, true, 50.0, 2.0, 5800.0)];
        let v = judged(&[(1, rep(&rows, 1)), (2, rep(&rows, 2))]).expect("a verdict");
        assert_eq!((v.winner, v.time, v.seen, v.reports, v.next, v.length), (2, 21.0, 2, 2, Some((1, 50.0)), Some(5800.0)));
        assert!(v.avg.unwrap() * 3.6 > 990.0, "{v:?}");
        assert_eq!(v.drove, None, "it came the whole way");
    }

    // a teleport to the finish with nobody else over the line: nothing to measure it by, until the race was won
    // cleanly (two clients' reports, another finisher) 10 times by two winners at least, the quickest in 3:00; then a
    // win in 0:21 is under half of that
    #[test]
    fn a_win_far_under_the_races_quickest_is_a_cheat() {
        let mut x = Results::default();
        let tele = |lobby| {
            let rows = [(2, true, 21.0, 2.0, 400.0), (1, false, 51.0, 0.56, 1624.0)];
            [(1, rep_of(lobby, &rows, 1)), (2, rep_of(lobby, &rows, 2))]
        };
        assert_eq!(judged_in(&mut x, &tele(0xA1)), None, "no clean win yet");
        // (3 and 4 win in turn)
        let clean = |k: u32| {
            let (w, o) = if k.is_multiple_of(2) { (3, 4) } else { (4, 3) };
            let rows = [(w, true, 180.0 + k as f64, 2.0, 5800.0), (o, true, 185.0 + k as f64, 2.0, 5810.0)];
            [(3, rep_of(0x100 + k, &rows, 3)), (4, rep_of(0x100 + k, &rows, 4))]
        };
        for k in 0..9 {
            assert_eq!(judged_in(&mut x, &clean(k)), None);
        }
        assert_eq!(judged_in(&mut x, &tele(0xA2)), None, "9 clean wins: too few to go by");
        assert_eq!(judged_in(&mut x, &clean(9)), None);
        let v = judged_in(&mut x, &tele(0xA3)).expect("a verdict");
        assert_eq!((v.winner, v.quickest, v.third, v.length, v.next), (2, Some(180.0), None, None, None));
        let l = line(&v, &|id| format!("p{id}"));
        assert!(l.contains("in 0:21.0, under 50 % of the race's quickest clean win (3:00.0); nobody else finished"), "{l}");
        // (a win alone, by one report, or by the reports of one client, is no clean win: it says nothing of the race;
        // nor do 10 clean wins of one winner)
        let mut y = Results::default();
        let alone = [(3, true, 180.0, 2.0, 5800.0), (4, false, 210.0, 1.5, 4300.0)];
        let both = [(3, true, 180.0, 2.0, 5800.0), (4, true, 185.0, 2.0, 5810.0)];
        for k in 0..10 {
            judged_in(&mut y, &[(3, rep_of(0x200 + k, &alone, 3)), (4, rep_of(0x200 + k, &alone, 4))]);
            y.add(0.0, 3, client(3), rep_of(0x300 + k, &both, 3), &|_| None);
            assert_eq!(y.due(RESULTS_WAIT_MS), vec![]);
            for by in [3, 4] {
                y.add(0.0, by, "one".into(), rep_of(0x400 + k, &both, by), &|_| None);
            }
            assert_eq!(y.due(0.0), vec![]);
        }
        assert!(y.best.is_empty());
        assert_eq!(judged_in(&mut y, &tele(0xA4)), None);
        let mut z = Results::default();
        for k in 0..10 {
            judged_in(&mut z, &[(3, rep_of(0x500 + k, &both, 3)), (4, rep_of(0x500 + k, &both, 4))]);
        }
        assert_eq!(judged_in(&mut z, &tele(0xA5)), None, "10 clean wins, all 3's");
    }

    // a car faster than tuning makes one (nfsu2.xyz, 2026-10-30: a sprint in 51.9 s, the third quickest of 32 wins
    // before 89.9 s): once the race had 10 clean wins, a win under 70 % of the third quickest other winner's; a
    // winner's own wins are not the others' to go by
    #[test]
    fn a_win_far_under_the_third_quickest_winners_is_a_cheat() {
        let mut x = Results::default();
        // (the next finisher at `next`: far behind, else a time that close can be driven)
        let race = |x: &mut Results, lobby: u32, w: u32, t: f64, next: f64| {
            let rows = [(w, true, t, 1.0, 5000.0), (99, true, next, 1.0, 5000.0)];
            judged_in(x, &[(w, rep_of(lobby, &rows, w)), (99, rep_of(lobby, &rows, 99))])
        };
        for k in 0..9 {
            assert_eq!(race(&mut x, 0x100 + k, 10 + k, 100.0 + k as f64, 105.0 + k as f64), None);
        }
        assert_eq!(race(&mut x, 0x200, 30, 60.0, 65.0), None, "9 clean wins: too few to go by");
        // 10 wins now: the quickest winners 60, 100 and 101 s, and 69 s is under 70 % of the third
        let v = race(&mut x, 0x201, 31, 69.0, 99.0).expect("a verdict");
        assert_eq!((v.third, v.quickest), (Some((101.0, 10)), None));
        let l = line(&v, &|id| format!("p{id}"));
        assert!(l.contains("under 70 % of the third quickest other winner's time of the race's 10 clean wins (1:41.0)"), "{l}");
        assert_eq!(race(&mut x, 0x202, 32, 71.0, 99.0), None, "71 s is over 70 % of 101 s");
        // a next finisher close behind (over 70 %): that time can be driven
        assert_eq!(race(&mut x, 0x204, 33, 64.0, 66.0), None, "64 s with another car in 66 s");
        // 30, whose 60 s came in before there were 10 wins, wins again in 62 s: the others' quickest are 64, 71 and
        // 100 s, not its own
        let v = race(&mut x, 0x203, 30, 62.0, 99.0).expect("a verdict");
        assert_eq!(v.third, Some((100.0, 12)));
    }

    // what the winner's own game said, on the line when it does not agree (nfsu2.xyz, 2026-10-31: three of five games
    // had a stock car win a 19.2 km circuit in 49 s, its own game said it did not finish), or sent no report
    #[test]
    fn the_line_says_what_the_winners_own_game_said() {
        let seen = [(2, true, 49.0, 3.0, 3000.0), (1, true, 401.8, 3.0, 19200.0), (3, true, 431.0, 3.0, 19210.0)];
        let own = [(2, false, 59.0, 0.4, 3000.0), (1, true, 401.8, 3.0, 19200.0), (3, true, 431.0, 3.0, 19210.0)];
        let mut x = Results::default();
        let v = judged_in(&mut x, &[(1, rep(&seen, 1)), (3, rep(&seen, 3)), (2, rep(&own, 2))]).expect("a verdict");
        assert_eq!((v.winner, v.seen, v.reports, v.own), (2, 2, 3, Some(OwnGame::Out)));
        let l = line(&v, &|id| format!("p{id}"));
        assert!(l.ends_with(": as 2 of 3 players' games saw it; its own game: did not finish"), "{l}");
        // (two reports of three: judged 90 s on)
        for by in [1, 3] {
            x.add(0.0, by, client(by), rep_of(0xB1, &seen, by), &|_| None);
        }
        let v = x.due(RESULTS_WAIT_MS).pop().expect("a verdict");
        assert!(line(&v, &|id| format!("p{id}")).ends_with(": as 2 of 2 players' games saw it; its own game sent no report"), "{v:?}");
        let v = judged_in(&mut x, &[1, 2, 3].map(|by| (by, rep_of(0xB2, &seen, by)))).expect("a verdict");
        assert!(line(&v, &|id| format!("p{id}")).ends_with(": as 3 of 3 players' games saw it"), "agrees: nothing more");
    }

    // a race whose other racers quit, by the one report that came: no line (nfsu2.xyz, 2026-10-30, a drag whose host
    // quit had the other racer first at 14.6 % of the way, by its game alone)
    #[test]
    fn one_report_of_a_race_the_others_quit_is_none() {
        let rows = [(2, false, 36.4, 0.146, 60.0), (1, false, 66.4, 0.0, 0.0)];
        let mut r = rep(&rows, 2);
        r.mode = 2;
        r.laps = 1;
        assert_eq!(judged(&[(2, r)]), None);
    }

    // the game ended Bob's race at once (a test's racewin, its real rows): first in 4.76 s after 0.6 % of the way, Ann
    // 30 s later at 16.8 % of the sprint; nobody finished by the rows' count, and the game's first is the winner
    #[test]
    fn a_race_the_game_ended_at_once_is_a_cheat() {
        let rows = [(2, false, 4.76, 0.006, 38.0), (1, false, 34.76, 0.168, 901.0)];
        let mut r = (rep(&rows, 1), rep(&rows, 2));
        r.0.laps = 1;
        r.1.laps = 1;
        let v = judged(&[(1, r.0), (2, r.1)]).expect("a verdict");
        assert_eq!((v.winner, v.time, v.next, v.length), (2, 4.76, None, None));
        assert!((v.drove.unwrap() - 0.006).abs() < 1e-9, "{v:?}");
        let l = line(&v, &|id| format!("p{id}"));
        assert!(l.contains("in 0:04.8, having driven 0.6 % of the way; nobody else finished"), "{l}");
    }

    // an honest race (a real one's numbers: 186.82 s and 187.67 s, 5.8 km), an honest winner over a racer who stood
    // still after 10 % of the way, and one over a racer who went off at 20 %: none
    #[test]
    fn honest_races_are_none() {
        let close = [(1, true, 186.82, 2.0, 5822.0), (2, true, 187.67, 2.0, 5838.0)];
        assert_eq!(judged(&[(1, rep(&close, 1)), (2, rep(&close, 2))]), None);
        // (a test's free-roam sprint, its real rows: 176.49 s and 178.15 s, 5265 m and 5281 m)
        let sprint = [(2, true, 176.49, 1.0, 5265.0), (1, true, 178.15, 1.0, 5281.0)];
        assert_eq!(judged(&[(1, rep(&sprint, 1)), (2, rep(&sprint, 2))]), None);
        let afk = [(2, true, 180.0, 2.0, 5800.0), (1, false, 210.0, 0.2, 580.0)];
        assert_eq!(judged(&[(1, rep(&afk, 1)), (2, rep(&afk, 2))]), None);
        assert_eq!(judged(&[(1, rep(&afk, 1))]), None, "the loser's report alone");
        let lost = [(2, true, 180.0, 2.0, 5800.0), (1, false, 210.0, 0.4, 1160.0), (3, true, 200.0, 2.0, 5810.0)];
        assert_eq!(judged(&[(1, rep(&lost, 1)), (2, rep(&lost, 2)), (3, rep(&lost, 3))]), None);
    }

    // nfsu2.xyz, 2026-10-29: QUAD69 won a sprint in 82.7 s (quick, and honest), the two others still on the way 30 s
    // later, driven far further than how far they came says (8.8 km for the race, scaled up): none
    #[test]
    fn racers_still_on_the_way_say_nothing_of_the_length() {
        let rows = [(2, true, 82.71, 1.0, 4600.0), (1, false, 112.71, 0.8, 7000.0), (3, false, 112.71, 0.75, 6600.0)];
        let mut r = [(1, rep(&rows, 1)), (2, rep(&rows, 2)), (3, rep(&rows, 3))];
        for x in &mut r {
            x.1.laps = 1;
        }
        assert_eq!(judged(&r), None);
    }

    // a next finisher far behind is no line: Mallory (3) says she finished 1000 s after Bob (2), whose own game and
    // Ann's say she did not finish
    #[test]
    fn a_next_finisher_far_behind_says_nothing() {
        let honest = [(2, true, 180.0, 2.0, 5800.0), (3, false, 210.0, 0.3, 900.0)];
        let fake = [(2, true, 180.0, 2.0, 5800.0), (3, true, 1000.0, 2.0, 5800.0)];
        assert_eq!(judged(&[(2, rep(&honest, 2)), (3, rep(&fake, 3))]), None);
    }

    // one report alone cannot name a winner (Mallory, 3: says Ann won in 10 s) nor make one's time quicker
    #[test]
    fn one_report_frames_nobody() {
        let honest = [(2, true, 180.0, 2.0, 5800.0), (1, true, 185.0, 2.0, 5800.0), (3, true, 190.0, 2.0, 5800.0)];
        let framed = [(1, true, 10.0, 2.0, 5800.0), (2, true, 180.0, 2.0, 5800.0), (3, true, 190.0, 2.0, 5800.0)];
        assert_eq!(judged(&[(1, rep(&honest, 1)), (2, rep(&honest, 2)), (3, rep(&framed, 3))]), None);
        let quick = [(2, true, 20.0, 2.0, 5800.0), (1, false, 50.0, 1.0, 2900.0)];
        let real = [(2, true, 180.0, 2.0, 5800.0), (1, true, 185.0, 2.0, 5800.0)];
        assert_eq!(judged(&[(1, rep(&quick, 1)), (2, rep(&real, 2))]), None, "the slowest time any report gave him");
    }

    // a drift race is left out; a race is judged once every racer's report came, else 90 s after the first
    #[test]
    fn drifts_are_left_out_and_races_wait() {
        let rows = [(2, false, 4.76, 0.006, 38.0), (1, false, 34.76, 0.168, 901.0)];
        let mut d = rep(&rows, 1);
        d.mode = MODE_DRIFT;
        assert_eq!(judged(&[(1, d)]), None);
        // (three racers, two reports: the game's end of the race needs two)
        let rows = [(2, false, 4.76, 0.006, 38.0), (1, false, 34.76, 0.168, 901.0), (3, false, 34.76, 0.15, 800.0)];
        let mut x = Results::default();
        x.add(0.0, 1, client(1), rep(&rows, 1), &|id| Some(format!("p{id}")));
        x.add(0.0, 3, client(3), rep(&rows, 3), &|id| Some(format!("p{id}")));
        assert_eq!(x.due(80_000.0), vec![], "two of three reports, 80 s on");
        let v = x.due(90_000.0);
        assert_eq!(v.len(), 1, "90 s on");
        assert_eq!(v[0].names.get(&2).map(String::as_str), Some("p2"), "the racers as the hub knew them then");
        assert_eq!(x.due(90_000.0), vec![], "judged once");
    }

    /// a report like `rep_of`'s, of heat `heat` of 3, with each row's jump (by the rows' order)
    fn heat_of(lobby: u32, heat: u8, rows: &[(u32, bool, f64, f64, f64)], jumps: &[f64], by: u32) -> RaceRes {
        let mut r = rep_of(lobby, rows, by);
        (r.heat, r.heats) = (heat, if heat > 0 { 3 } else { 0 });
        for (k, &j) in jumps.iter().enumerate() {
            if r.rows[k].id != by {
                r.rows[k].jump = j;
            }
        }
        r
    }

    // nfsu2.xyz, 2026-11-04: a URL race of 3 heats; in heat 2 one car (1) jumped 279.9 m once by the host's game, the
    // others' jumps 44 m at the most: a line of its own when two other games had it, or the only other one; none for
    // one of two, nor for jumps a reset or a stall makes
    #[test]
    fn a_car_that_jumped_is_a_line() {
        let who = |id: u32| format!("10.77.0.{id} \"P{id}\"");
        let rows = [(1, true, 187.88, 3.0, 9503.0), (2, true, 206.35, 3.0, 9657.0), (3, false, 217.88, 2.97, 9524.0)];
        let mut x = Results::default();
        for (by, j) in [(1, [0.0, 3.2, 7.6]), (2, [279.9, 0.0, 7.6]), (3, [275.0, 3.2, 0.0])] {
            x.add(0.0, by, client(by), heat_of(0xC1, 2, &rows, &j, by), &|id| Some(who(id)));
        }
        let j = x.judged(0.0);
        assert_eq!(j.len(), 1);
        let (o, v) = &j[0];
        assert_eq!((v, o.heat, o.heats), (&None, 2, 3), "no time rule: 1 won by 18 s");
        assert_eq!(o.jumps, vec![Jump { id: 1, seen: vec![279.9, 275.0], others: 2, place: Some(1) }]);
        let l = jump_line(o, &o.jumps[0], &who);
        assert_eq!(
            l,
            "[cheat] 10.77.0.1 \"P1\": its car jumped 280 m at once in the race of lobby 000000c1 (race 0badf00d: sprint, 2 \
             laps, heat 2 of 3), as 2 of 2 other players' games had it (the largest each: 280, 275 m; honest races' largest \
             are under 50 m); it won"
        );
        assert!(l.contains(" the race of lobby "), "a race's line: the operator's handle goes after it (control.rs)");
        assert!(result_line(o, &who).starts_with("[race] lobby 000000c1, race 0badf00d (sprint, 2 laps, heat 2 of 3): "));
        // one of two other games: none
        let mut x = Results::default();
        for (by, j) in [(1, [0.0, 3.2, 7.6]), (2, [279.9, 0.0, 7.6]), (3, [40.0, 3.2, 0.0])] {
            x.add(0.0, by, client(by), heat_of(0xC2, 2, &rows, &j, by), &|_| None);
        }
        assert_eq!(x.judged(0.0)[0].0.jumps, vec![]);
        // two racers: the only other game; the racer's own report says nothing of its own car
        let two = [(2, true, 180.0, 2.0, 5800.0), (1, false, 210.0, 1.5, 4300.0)];
        let mut x = Results::default();
        x.add(0.0, 2, client(2), heat_of(0xC3, 0, &two, &[0.0, 0.0], 2), &|_| None);
        x.add(0.0, 1, client(1), heat_of(0xC3, 0, &two, &[400.0, 0.0], 1), &|_| None);
        let (o, _) = &x.judged(0.0)[0];
        assert_eq!(o.jumps, vec![Jump { id: 2, seen: vec![400.0], others: 1, place: Some(1) }]);
        assert!(jump_line(o, &o.jumps[0], &|id| format!("p{id}")).contains("(race 0badf00d: sprint, 2 laps), as 1 of 1 other"));
        // honest jumps (a reset to the road, a hit): none
        let mut x = Results::default();
        for (by, j) in [(1, [0.0, 44.0, 31.0]), (2, [39.5, 0.0, 42.9]), (3, [44.1, 7.6, 0.0])] {
            x.add(0.0, by, client(by), heat_of(0xC4, 1, &rows, &j, by), &|_| None);
        }
        assert_eq!(x.judged(0.0)[0].0.jumps, vec![]);
    }

    // a race's heats are races of their own: judged apart (a report a heat), their clean wins apart (another track each)
    #[test]
    fn heats_are_judged_apart() {
        let mut x = Results::default();
        let h1 = [(1, true, 93.94, 3.0, 4932.0), (2, true, 94.77, 3.0, 4990.0)];
        let h2 = [(1, true, 187.88, 3.0, 9503.0), (2, true, 206.35, 3.0, 9657.0)];
        for by in [1, 2] {
            x.add(0.0, by, client(by), heat_of(0xD1, 1, &h1, &[], by), &|_| None);
            x.add(0.0, by, client(by), heat_of(0xD1, 2, &h2, &[], by), &|_| None);
        }
        let mut j = x.judged(0.0);
        j.sort_by_key(|(o, _)| o.heat);
        assert_eq!(
            j.iter().map(|(o, v)| (o.heat, o.order[0], v.is_none())).collect::<Vec<_>>(),
            vec![(1, (1, Some(93.94)), true), (2, (1, Some(187.88)), true)]
        );
        assert_eq!(x.best.keys().map(|k| k.2).collect::<std::collections::BTreeSet<_>>(), [1, 2].into(), "each heat's own");
    }

    // nfsu2.xyz, 2026-11-02: the race's quickest clean win 7:08.4 (the first after a restart), then wins in 2:24-2:53
    // with the next car 1-14 % behind, each under half of it: two cars that close cannot both have cheated; none
    #[test]
    fn a_close_finish_is_no_time_cheat() {
        let mut x = Results::default();
        // (10 clean wins to go by, 3 and 4 in turn, 7:08.4 the quickest)
        for k in 0..10 {
            let (w, o) = if k % 2 == 0 { (3, 4) } else { (4, 3) };
            let slow = [(w, true, 428.4 + k as f64, 2.0, 5700.0), (o, true, 440.0 + k as f64, 2.0, 5710.0)];
            assert_eq!(judged_in(&mut x, &[(3, rep_of(0xE100 + k, &slow, 3)), (4, rep_of(0xE100 + k, &slow, 4))]), None);
        }
        let close = [(1, true, 144.4, 2.0, 5700.0), (2, true, 145.9, 2.0, 5700.0)];
        assert_eq!(judged_in(&mut x, &[(1, rep_of(0xE2, &close, 1)), (2, rep_of(0xE2, &close, 2))]), None);
        let behind = [(5, true, 129.6, 2.0, 5700.0), (6, true, 151.1, 2.0, 5700.0)];
        assert_eq!(judged_in(&mut x, &[(5, rep_of(0xE3, &behind, 5)), (6, rep_of(0xE3, &behind, 6))]), None, "13 % behind");
        // (a winner far ahead still is one)
        let far = [(7, true, 60.0, 2.0, 5700.0), (8, true, 140.0, 2.0, 5700.0)];
        assert!(judged_in(&mut x, &[(7, rep_of(0xE4, &far, 7)), (8, rep_of(0xE4, &far, 8))]).is_some());
        // an average no car makes, with the next car as close: the race's length was off (nfsu2.xyz, 2026-11-01: 360
        // km/h over 12.7 km, the next 4 s behind)
        let long = [(1, true, 127.4, 2.0, 12700.0), (2, true, 131.7, 2.0, 12700.0)];
        assert_eq!(judged(&[(1, rep(&long, 1)), (2, rep(&long, 2))]), None);
    }

    // nfsu2.xyz, 2026-11-01..04: 2-car drags "won" in 5-7 s at 3.5 % of the way, by the winner's game alone (the other
    // racer quit, the game ended the race): no time rule, however quick
    #[test]
    fn a_race_the_others_quit_is_no_time_cheat() {
        let mut x = Results::default();
        let drag = |lobby: u32, rows: &[(u32, bool, f64, f64, f64)], by: u32| RaceRes { laps: 1, ..rep_of(lobby, rows, by) };
        // (10 clean wins to go by, 3 and 4 in turn)
        for k in 0..10 {
            let (w, o) = if k % 2 == 0 { (3, 4) } else { (4, 3) };
            let clean = [(w, true, 23.8, 1.0, 1600.0), (o, true, 26.6, 1.0, 1600.0)];
            assert_eq!(judged_in(&mut x, &[(3, drag(0xF0 + k, &clean, 3)), (4, drag(0xF0 + k, &clean, 4))]), None);
        }
        let quit = [(1, false, 5.1, 0.035, 56.0), (2, false, 35.1, 0.0, 0.0)];
        x.add(0.0, 1, client(1), drag(0xFF, &quit, 1), &|_| None);
        assert_eq!(x.due(RESULTS_WAIT_MS), vec![], "the one report, 90 s on");
        // (by two games it is the game's end of the race: a line, as before)
        assert!(judged_in(&mut x, &[(1, drag(0xFE, &quit, 1)), (2, drag(0xFE, &quit, 2))]).is_some_and(|v| v.drove.is_some()));
    }
}
