//! What the race checks of the free-roam hub (nfsu2-roam: results.rs, hub.rs) and of the Online mode (nfsu2-online:
//! lobby.rs) share: the limits no honest race passes, the rule for a car's jump, and a track's quickest racers
//! ([`Quickest`]), which a win far quicker than theirs is measured against.  What differs stays with each check: what
//! makes a race clean, whose times it adds (the hub's: the winner's; the Online mode's: every finisher's), and the
//! hub's own quickest clean win.

/// no car of the game averages this over a race (m/s: 350 km/h; the fastest tuned cars make 350 at the most, a race's
/// average far less: a 2-lap circuit's was 112 km/h)
pub const RACE_MPS: f64 = 350.0 / 3.6;

/// a top speed no car of the game makes, however tuned (km/h)
pub const CAR_KMH: f64 = 420.0;

/// A winner in under THIRD_SHARE of the third quickest other racer's time of its track ([`Quickest::third`]), once the
/// track had QUICK_WINS clean races, is a `[cheat]` line: a car made faster than any tuning makes one.  Not when another
/// finisher came close (the winner over THIRD_SHARE of its time): that time can be driven.  TOP_KEPT: the quickest
/// racers kept of each track.
pub const THIRD_SHARE: f64 = 0.7;
pub const QUICK_WINS: u32 = 10;
pub const TOP_KEPT: usize = 4;

/// A car's jump at once (m) that no lag makes, and how many of the other racers' games must have had it so (or the only
/// other one: [`jump_seen`]).  Honest races' largest: 18-44 m (a reset to the road, a hit), 31 m after a 1.5 s stall
/// of a test's (runtime/racenet.c); a car a player said stood still for laps, then finished at once, jumped 279.9 m in
/// the reporter's game.
pub const JUMP_M: f64 = 150.0;
pub const JUMP_SEEN: usize = 2;

/// a drift race's mode (roam_core.h's race mode, the Online mode's race type): won by points, so left out of the time
/// rules
pub const MODE_DRIFT: u8 = 3;

/// whether the other racers' games had a car jump: `far` of them had it JUMP_M or more, JUMP_SEEN at least, or the
/// one when it is the only other one (`only_other`)
pub fn jump_seen(far: usize, only_other: bool) -> bool {
    far >= JUMP_SEEN || (only_other && far == 1)
}

/// A track's clean races: how many (`wins`, which the check counts as it sees fit), and its quickest racers, each once
/// by name with its best time, quickest first, TOP_KEPT at most
#[derive(Clone, Debug, Default)]
pub struct Quickest {
    pub wins: u32,
    top: Vec<(String, f64)>,
}

impl Quickest {
    /// a racer's time (its best is kept); `wins` is left as it is
    pub fn add(&mut self, who: String, time: f64) {
        match self.top.iter_mut().find(|(w, _)| *w == who) {
            Some(x) => x.1 = x.1.min(time),
            None => self.top.push((who, time)),
        }
        self.top.sort_by(|a, b| a.1.total_cmp(&b.1));
        self.top.truncate(TOP_KEPT);
    }

    /// the racers kept, quickest first
    pub fn top(&self) -> &[(String, f64)] {
        &self.top
    }

    /// the third quickest racer's time but `who`'s, once the track had QUICK_WINS clean races
    pub fn third(&self, who: &str) -> Option<f64> {
        (self.wins >= QUICK_WINS).then(|| self.top.iter().filter(|(w, _)| w != who).nth(2).map(|x| x.1)).flatten()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_quickest_keep_each_racers_best_and_the_third_leaves_the_winner_out() {
        let mut q = Quickest::default();
        for (who, t) in [("a", 60.0), ("b", 62.0), ("c", 65.0), ("a", 58.0), ("d", 70.0), ("e", 71.0)] {
            q.add(who.into(), t);
        }
        let names: Vec<(&str, f64)> = q.top().iter().map(|(w, t)| (w.as_str(), *t)).collect();
        assert_eq!(names, [("a", 58.0), ("b", 62.0), ("c", 65.0), ("d", 70.0)], "each once, its best, TOP_KEPT");
        assert_eq!(q.third("x"), None, "not before QUICK_WINS");
        q.wins = QUICK_WINS;
        assert_eq!(q.third("x"), Some(65.0));
        assert_eq!(q.third("a"), Some(70.0), "the winner's own time is not one to beat");
        q.add("a".into(), 80.0);
        assert_eq!(q.top()[0], ("a".to_string(), 58.0), "a slower time leaves the best");
    }

    #[test]
    fn a_jump_counts_by_two_games_or_the_only_other_one() {
        assert!(jump_seen(2, false) && jump_seen(3, true));
        assert!(!jump_seen(1, false) && jump_seen(1, true));
        assert!(!jump_seen(0, true));
    }
}
