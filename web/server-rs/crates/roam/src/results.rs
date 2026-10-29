//! The races' results as the players' games showed them (RACERES, roam_core.h rc_raceres), for a race won by
//! cheating: a teleport to the finish, a speed hack.  Every racer's game sends its results once back in free roam,
//! each row by the racer's player id: finished or not, its time, how far it came (laps), the way it drove (m).  The
//! hub keeps a race's reports until every racer's came, or RESULTS_WAIT_MS after the first, then judges it once.
//!
//! - the winner: the one who came first in more than half of the reports (one report cannot name a winner the others
//!   do not), at the slowest time any report gave it (one report's time cannot make it quicker).  A report's first is
//!   its quickest finisher; with none (none came the race's whole way), the game's own first row when every other one
//!   ended with the race, GAME_END_S or more after it: the game finished it without the way driven, as a cheat that
//!   ends its race at once does (a test's racewin, 2026-10-28: first in 4.76 s after 0.6 % of the way, the other
//!   racer 30 s later at 17 %)
//! - the race's length: what the others drove in it, each by its own game (its own row: the way it drove, for as far
//!   as it came: a racer still on the way when the race ended, 30 s after the first finished, counts for the whole
//!   way at its own pace), the shortest of them; never the winner's own report
//! - a `[cheat]` line when the winner averaged RACE_MPS (350 km/h) or more over that length, which no car of the game
//!   does (the fastest tuned cars make 350 at the most, a race's average far less: a 2-lap circuit's was 112 km/h),
//!   or when it finished in under FAR_AHEAD (40 %) of another finisher's time.  Only finishers count for that: a racer
//!   who stood still or went away from the keyboard finished nothing, and its pace says nothing of the winner's.
//!
//! A drift race is won by points and is left out.  The lines are for the operator (`nfsu2-server ctl kick|ban`), and
//! say how many of the race's reports saw it.

use std::collections::HashMap;

use crate::wire::{LOBBY_PLAYERS, RS_FINISHED, RS_OURS, RaceRes, ResRow};

/// a race's reports are judged once every racer's came, or this long after the first (ms): every game sends its
/// report back in free roam, the host's 4 s after the others' (mprace.c)
pub(crate) const RESULTS_WAIT_MS: f64 = 90_000.0;
/// the races kept at once (a player's reports are one a race, none sooner than 20 s after its last: hub.rs)
const RACES_KEPT: usize = 4096;
/// no car of the game averages this over a race (m/s: 350 km/h)
pub(crate) const RACE_MPS: f64 = 350.0 / 3.6;
/// a winner in under this share of another finisher's time
pub(crate) const FAR_AHEAD: f64 = 0.4;
/// a racer who came less than this share of the way says nothing of the race's length
const LENGTH_MIN_DONE: f64 = 0.1;
/// roam_core.h's race mode of a drift race
const MODE_DRIFT: u8 = 3;
/// a race ends 30 s after its first finished (mprace.c): the others' rows are that much later, this much at least (s)
const GAME_END_S: f64 = 25.0;
/// a winner who drove less than this share of the way: the line says how much it drove
const DROVE_SHORT: f64 = 0.9;

/// one race's reports so far: since when, each reporter's, and its racers as the hub knew them then (their address and
/// name, for the line: they may be gone by the time the race is judged)
struct Race {
    at: f64,
    reports: Vec<(u32, RaceRes)>,
    names: HashMap<u32, String>,
}

/// What a race's reports showed of its winner, when it is a cheat's
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Verdict {
    pub lobby: u32,
    pub race: u32,
    pub mode: u8,
    pub laps: u8,
    /// the winner's player id and time (s)
    pub winner: u32,
    pub time: f64,
    /// the race's length by the others' games (m), and the winner's average over it (m/s)
    pub length: Option<f64>,
    pub avg: Option<f64>,
    /// the next finisher (its id and time), if any
    pub next: Option<(u32, f64)>,
    /// the share of the way the winner drove, the most any report gave it, when that is under DROVE_SHORT
    pub drove: Option<f64>,
    /// the reports that had it first, and all of them
    pub seen: usize,
    pub reports: usize,
    /// its racers as the hub knew them when their reports came (`Results::add`)
    pub names: HashMap<u32, String>,
}

/// The races' reports, until each is judged
#[derive(Default)]
pub(crate) struct Results {
    races: HashMap<u32, Race>,
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

/// A verdict's `[cheat]` line; `who`: a player's address and name by its id (`10.77.3.41 "Bob"`)
pub(crate) fn line(v: &Verdict, who: &dyn Fn(u32) -> String) -> String {
    let mut s = format!(
        "[cheat] {}: won the race of lobby {:08x} (race {:08x}: {}, {} lap{}) in {}",
        who(v.winner),
        v.lobby,
        v.race,
        mode_name(v.mode),
        v.laps,
        if v.laps == 1 { "" } else { "s" },
        clock(v.time)
    );
    match (v.length, v.avg) {
        (Some(l), Some(a)) => s += &format!(
            ", {:.0} km/h on average over the race's {:.1} km (as the others drove it){}",
            a * 3.6,
            l / 1000.0,
            if a >= RACE_MPS { ", which no car does" } else { "" }
        ),
        _ => s += " (no other racer's game measured the race's length)",
    }
    if let Some(d) = v.drove {
        s += &format!(", having driven {:.1} % of the way", d * 100.0);
    }
    match v.next {
        Some((id, t)) => {
            let under = if v.time < FAR_AHEAD * t { format!("under {:.0} % of ", FAR_AHEAD * 100.0) } else { String::new() };
            s += &format!("; {under}the next finisher's {}, {}", clock(t), who(id));
        }
        None => s += "; nobody else finished (the race ends 30 s after the first)",
    }
    s + &format!(": as {} of {} players' games saw it", v.seen, v.reports)
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
    /// a player's report of a race (one a reporter: a second one of it counts for nothing), and `who` its racers by id
    /// as the hub knows them now; false when there is no room for a race more
    pub(crate) fn add(&mut self, now: f64, reporter: u32, r: RaceRes, who: &dyn Fn(u32) -> Option<String>) -> bool {
        if !self.races.contains_key(&r.lobby) && self.races.len() >= RACES_KEPT {
            return false;
        }
        let race = self.races.entry(r.lobby).or_insert_with(|| Race { at: now, reports: Vec::new(), names: HashMap::new() });
        if !race.reports.iter().any(|(id, _)| *id == reporter) && race.reports.len() < LOBBY_PLAYERS {
            race.reports.push((reporter, r));
            for x in &r.rows[..r.n] {
                if x.id != 0 && !race.names.contains_key(&x.id) && let Some(w) = who(x.id) {
                    race.names.insert(x.id, w);
                }
            }
        }
        true
    }

    /// the races due (every racer's report came, or RESULTS_WAIT_MS went by), judged and forgotten: the cheats' verdicts
    pub(crate) fn due(&mut self, now: f64) -> Vec<Verdict> {
        let due: Vec<u32> = self
            .races
            .iter()
            .filter(|(_, r)| {
                let racers = r.reports.iter().map(|(_, x)| x.n).max().unwrap_or(0);
                now - r.at >= RESULTS_WAIT_MS || r.reports.len() >= racers
            })
            .map(|(&l, _)| l)
            .collect();
        due.into_iter()
            .filter_map(|l| {
                let race = self.races.remove(&l)?;
                judge(&race.reports).map(|v| Verdict { names: race.names, ..v })
            })
            .collect()
    }
}

/// One race's reports judged: its winner's verdict, when it is a cheat's
fn judge(reports: &[(u32, RaceRes)]) -> Option<Verdict> {
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
    // the race's length: each other racer's own way for as far as it came, the shortest
    let length = reports
        .iter()
        .filter(|(by, _)| *by != w)
        .filter_map(|(by, r)| {
            let far = whole_way(r);
            let own = r.rows[..r.n].iter().find(|x| x.flags & RS_OURS != 0 && x.id == *by)?;
            (own.done >= LENGTH_MIN_DONE * far && own.dist > 0.0).then(|| own.dist * far / own.done.min(far))
        })
        .reduce(f64::min);
    let avg = length.map(|l| l / time);
    // the next finisher: the quickest other one any report has
    let next = reports
        .iter()
        .flat_map(|(_, r)| r.rows[..r.n].iter().filter(|x| x.id != w && x.flags & RS_FINISHED != 0 && x.time > 0.0).map(|x| (x.id, x.time)))
        .min_by(|a, b| a.1.total_cmp(&b.1));
    let fast = avg.is_some_and(|a| a >= RACE_MPS);
    let ahead = next.is_some_and(|(_, t)| time < FAR_AHEAD * t);
    (fast || ahead).then_some(Verdict {
        lobby: first.lobby,
        race: first.race,
        mode: first.mode,
        laps: first.laps,
        winner: w,
        time,
        length,
        avg,
        next,
        drove: (drove < DROVE_SHORT).then_some(drove),
        seen,
        reports: reports.len(),
        names: HashMap::new(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// a report of lobby 0xA1 (a 2-lap sprint) by `by`: rows (id, finished, time, laps, m)
    fn rep(rows: &[(u32, bool, f64, f64, f64)], by: u32) -> RaceRes {
        let mut r = RaceRes { lobby: 0xA1, race: 0x0BAD_F00D, mode: 1, laps: 2, n: rows.len(), rows: [ResRow::default(); LOBBY_PLAYERS] };
        for (k, &(id, fin, time, done, dist)) in rows.iter().enumerate() {
            let flags = if fin { RS_FINISHED } else { 0 } | if id == by { RS_OURS } else { 0 };
            r.rows[k] = ResRow { id, flags, time, done, dist };
        }
        r
    }

    fn judged(reports: &[(u32, RaceRes)]) -> Option<Verdict> {
        let mut x = Results::default();
        for (by, r) in reports {
            x.add(0.0, *by, *r, &|_| None);
        }
        let mut v = x.due(0.0);
        assert!(v.len() <= 1);
        v.pop()
    }

    // a teleport to the finish: Bob (2) in 21 s, Ann (1) 28 % of the 5.8 km on when the race ended; Bob's own report
    // says the same
    #[test]
    fn a_win_no_car_makes_is_a_cheat() {
        let rows = [(2, true, 21.0, 2.0, 400.0), (1, false, 51.0, 0.56, 1624.0)];
        let v = judged(&[(1, rep(&rows, 1)), (2, rep(&rows, 2))]).expect("a verdict");
        assert_eq!((v.winner, v.time, v.seen, v.reports, v.next), (2, 21.0, 2, 2, None));
        assert!((v.length.unwrap() - 5800.0).abs() < 1.0, "{v:?}");
        assert!(v.avg.unwrap() * 3.6 > 990.0, "{v:?}");
        assert_eq!(v.drove, None, "it came the whole way");
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
        assert_eq!((v.winner, v.time, v.next), (2, 4.76, None));
        assert!((v.length.unwrap() - 5363.0).abs() < 1.0 && v.avg.unwrap() > 1000.0, "{v:?}");
        assert!((v.drove.unwrap() - 0.006).abs() < 1e-9, "{v:?}");
        let l = line(&v, &|id| format!("p{id}"));
        assert!(l.contains("having driven 0.6 % of the way") && l.contains("nobody else finished"), "{l}");
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

    // a speed hack 3 times as fast: under 40 % of the next finisher's time (a car at 350 km/h, no faster)
    #[test]
    fn far_ahead_of_a_finisher_is_a_cheat() {
        let rows = [(2, true, 60.0, 2.0, 5800.0), (1, true, 170.0, 2.0, 5810.0)];
        let v = judged(&[(1, rep(&rows, 1)), (2, rep(&rows, 2))]).expect("a verdict");
        assert_eq!((v.winner, v.next), (2, Some((1, 170.0))));
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
        let rows = [(2, true, 21.0, 2.0, 400.0), (1, false, 51.0, 0.56, 1624.0)];
        let mut d = rep(&rows, 1);
        d.mode = MODE_DRIFT;
        assert_eq!(judged(&[(1, d)]), None);
        let mut x = Results::default();
        x.add(0.0, 1, rep(&rows, 1), &|id| Some(format!("p{id}")));
        assert_eq!(x.due(80_000.0), vec![], "one of two reports, 80 s on");
        let v = x.due(90_000.0);
        assert_eq!(v.len(), 1, "90 s on");
        assert_eq!(v[0].names.get(&2).map(String::as_str), Some("p2"), "the racers as the hub knew them then");
        assert_eq!(x.due(90_000.0), vec![], "judged once");
    }
}
