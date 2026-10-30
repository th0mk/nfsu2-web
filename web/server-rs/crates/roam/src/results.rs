//! The races' results as the players' games showed them (RACERES, roam_core.h rc_raceres), for a race won by
//! cheating: a teleport to the finish, a speed hack, a race the game ended at once.  Every racer's game sends its
//! results once back in free roam, each row by the racer's player id: finished or not, its time, how far it came
//! (laps), the way it drove (m).  The hub takes a report only from a player of the race's lobby, naming its players
//! (hub.rs `Joined`), one a lobby; it keeps a race's reports until every racer's came, or RESULTS_WAIT_MS after the
//! first, then judges it once.
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
//!   when it came under DROVE_MIN of the way by every report (the game ended its race); or when it won in under
//!   QUICK_SHARE of the race's quickest clean win the hub saw (`Results::best`: by two reports at least, with another
//!   finisher, no line), which a teleport to the finish with nobody else over the line does.  A next finisher far
//!   behind says nothing: the race ends 30 s after the first finished, so only a report the game did not make can show
//!   one.
//!
//! A drift race is won by points and is left out.  The lines are for the operator (`nfsu2-server ctl kick|ban`), and
//! say how many of the race's reports saw it.

use std::collections::HashMap;

use crate::wire::{LOBBY_PLAYERS, RS_FINISHED, RS_OURS, RaceRes, ResRow};

/// a race's reports are judged once every racer's came, or this long after the first (ms): every game sends its
/// report back in free roam, the host's 4 s after the others' (mprace.c)
pub(crate) const RESULTS_WAIT_MS: f64 = 90_000.0;
/// the races kept at once (a player's reports are one a lobby, none sooner than 20 s after its last: hub.rs)
const RACES_KEPT: usize = 4096;
/// no car of the game averages this over a race (m/s: 350 km/h)
const RACE_MPS: f64 = 350.0 / 3.6;
/// a winner who came less than this share of the way by every report finished without driving it
const DROVE_MIN: f64 = 0.5;
/// a winner in under this share of the race's quickest clean win; the clean wins kept (a race and its laps each), and
/// the reports that had a clean win's winner first, at least
const QUICK_SHARE: f64 = 0.5;
const BEST_KEPT: usize = 4096;
const BEST_SEEN: usize = 2;
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

/// What a race's reports showed of its winner
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Verdict {
    pub lobby: u32,
    pub race: u32,
    pub mode: u8,
    pub laps: u8,
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
    /// the reports that had it first, and all of them
    pub seen: usize,
    pub reports: usize,
    /// its racers as the hub knew them when their reports came (`Results::add`)
    pub names: HashMap<u32, String>,
}

/// The races' reports, until each is judged; the quickest clean win of each race (its name hash and laps) so far
#[derive(Default)]
pub(crate) struct Results {
    races: HashMap<u32, Race>,
    best: HashMap<(u32, u8), f64>,
}

/// a race's mode as a word (roam_core.h rc_racerep), in its game's words (`game`: roam_core.h RC_GAME_*).  Most
/// Wanted's mode is its own race type (GRaceType, the invite's GAMEMODE: 0 sprint, 1 circuit, 2 drag, 3 lap knockout,
/// 4 tollbooth, 5 speedtrap); Underground 2's its race mode (drift, URL and Street X are its own).
pub(crate) fn mode_name(game: u8, mode: u8) -> &'static str {
    if game == crate::GAME_MW {
        const MODES: [&str; 6] = ["sprint", "circuit", "drag", "lap knockout", "tollbooth", "speedtrap"];
        return MODES.get(mode as usize).copied().unwrap_or("race");
    }
    const MODES: [&str; 5] = ["circuit", "sprint", "drag", "drift", "URL"];
    MODES.get(mode as usize).copied().unwrap_or(if mode == 10 { "Street X" } else { "race" })
}

/// a time as m:ss.s
fn clock(s: f64) -> String {
    format!("{}:{:04.1}", (s / 60.0).floor(), s % 60.0)
}

/// A verdict's `[cheat]` line; `who`: a player's address and name by its id (`10.77.3.41 "Bob"`)
pub(crate) fn line(game: u8, v: &Verdict, who: &dyn Fn(u32) -> String) -> String {
    let mut s = format!(
        "[cheat] {}: won the race of lobby {:08x} (race {:08x}: {}, {} lap{}) in {}",
        who(v.winner),
        v.lobby,
        v.race,
        mode_name(game, v.mode),
        v.laps,
        if v.laps == 1 { "" } else { "s" },
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
    if let Some(d) = v.drove {
        s += &format!(", having driven {:.1} % of the way", d * 100.0);
    }
    match v.next {
        Some((id, t)) => s += &format!("; the next finisher's {}, {}", clock(t), who(id)),
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

    /// the races due (every racer's report came, or RESULTS_WAIT_MS went by), judged and forgotten: the cheats'
    /// verdicts.  A clean win by two reports or more, with another finisher, may be its race's quickest.
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
        let mut cheats = Vec::new();
        for l in due {
            let Some(race) = self.races.remove(&l) else { continue };
            let Some((v, cheat)) = judge(&race.reports, &self.best) else { continue };
            if cheat {
                cheats.push(Verdict { names: race.names, ..v });
                continue;
            }
            let k = (v.race, v.laps);
            if v.seen >= BEST_SEEN && v.next.is_some() && (self.best.len() < BEST_KEPT || self.best.contains_key(&k)) {
                let b = self.best.entry(k).or_insert(v.time);
                *b = b.min(v.time);
            }
        }
        cheats
    }
}

/// One race's reports judged (`best`: the races' quickest clean wins): its winner's verdict, and whether it is a
/// cheat's
fn judge(reports: &[(u32, RaceRes)], best: &HashMap<(u32, u8), f64>) -> Option<(Verdict, bool)> {
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
    let quickest = best.get(&(first.race, first.laps)).copied().filter(|&b| time < QUICK_SHARE * b);
    let cheat = avg.is_some_and(|a| a >= RACE_MPS) || drove < DROVE_MIN || quickest.is_some();
    let v = Verdict {
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
        quickest,
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
        let mut r = RaceRes { lobby, race: 0x0BAD_F00D, mode: 1, laps: 2, n: rows.len(), rows: [ResRow::default(); LOBBY_PLAYERS] };
        for (k, &(id, fin, time, done, dist)) in rows.iter().enumerate() {
            let flags = if fin { RS_FINISHED } else { 0 } | if id == by { RS_OURS } else { 0 };
            r.rows[k] = ResRow { id, flags, time, done, dist };
        }
        r
    }

    fn rep(rows: &[(u32, bool, f64, f64, f64)], by: u32) -> RaceRes {
        rep_of(0xA1, rows, by)
    }

    /// the cheats of the races of these reports, each judged once all its reports came
    fn judged_in(x: &mut Results, reports: &[(u32, RaceRes)]) -> Option<Verdict> {
        for (by, r) in reports {
            x.add(0.0, *by, *r, &|_| None);
        }
        let mut v = x.due(0.0);
        assert!(v.len() <= 1);
        v.pop()
    }

    fn judged(reports: &[(u32, RaceRes)]) -> Option<Verdict> {
        judged_in(&mut Results::default(), reports)
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
    // cleanly (two reports, another finisher) in 3:00; then a win in 0:21 is under half of that
    #[test]
    fn a_win_far_under_the_races_quickest_is_a_cheat() {
        let mut x = Results::default();
        let tele = |lobby| {
            let rows = [(2, true, 21.0, 2.0, 400.0), (1, false, 51.0, 0.56, 1624.0)];
            [(1, rep_of(lobby, &rows, 1)), (2, rep_of(lobby, &rows, 2))]
        };
        assert_eq!(judged_in(&mut x, &tele(0xA1)), None, "no clean win yet");
        let clean = [(3, true, 180.0, 2.0, 5800.0), (4, true, 185.0, 2.0, 5810.0)];
        assert_eq!(judged_in(&mut x, &[(3, rep_of(0xA2, &clean, 3)), (4, rep_of(0xA2, &clean, 4))]), None);
        let v = judged_in(&mut x, &tele(0xA3)).expect("a verdict");
        assert_eq!((v.winner, v.quickest, v.length, v.next), (2, Some(180.0), None, None));
        let l = line(0, &v, &|id| format!("p{id}"));
        assert!(l.contains("in 0:21.0, under 50 % of the race's quickest clean win (3:00.0); nobody else finished"), "{l}");
        // (a win alone, or by one report, is no clean win: it says nothing of the race)
        let mut y = Results::default();
        let alone = [(3, true, 180.0, 2.0, 5800.0), (4, false, 210.0, 1.5, 4300.0)];
        judged_in(&mut y, &[(3, rep_of(0xA2, &alone, 3)), (4, rep_of(0xA2, &alone, 4))]);
        judged_in(&mut y, &[(3, rep_of(0xA4, &clean, 3))]);
        assert_eq!(judged_in(&mut y, &tele(0xA3)), None);
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
        let l = line(0, &v, &|id| format!("p{id}"));
        assert!(l.contains("in 0:04.8, having driven 0.6 % of the way; nobody else finished"), "{l}");
    }

    // the modes in each game's words: Underground 2's as its [race] and [cheat] lines always had them, Most Wanted's
    // its own race types
    #[test]
    fn the_modes_in_each_games_words() {
        let ug2: Vec<&str> = (0..=11).map(|m| mode_name(crate::GAME_UG2, m)).collect();
        assert_eq!(ug2, ["circuit", "sprint", "drag", "drift", "URL", "race", "race", "race", "race", "race", "Street X", "race"]);
        let mw: Vec<&str> = (0..=7).map(|m| mode_name(crate::GAME_MW, m)).collect();
        assert_eq!(mw, ["sprint", "circuit", "drag", "lap knockout", "tollbooth", "speedtrap", "race", "race"]);
        assert_eq!(mode_name(crate::GAME_MW, 10), "race", "no Street X in Most Wanted");
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
        let mut x = Results::default();
        x.add(0.0, 1, rep(&rows, 1), &|id| Some(format!("p{id}")));
        assert_eq!(x.due(80_000.0), vec![], "one of two reports, 80 s on");
        let v = x.due(90_000.0);
        assert_eq!(v.len(), 1, "90 s on");
        assert_eq!(v[0].names.get(&2).map(String::as_str), Some("p2"), "the racers as the hub knew them then");
        assert_eq!(x.due(90_000.0), vec![], "judged once");
    }
}
