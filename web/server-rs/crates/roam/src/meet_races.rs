//! Deterministic meet-race rules. Times are monotonic server milliseconds; callers
//! authenticate seats and validate destinations before creating an event. No I/O,
//! client finish claims, or career mutations belong in this state machine.
use std::collections::BTreeMap;

pub const PERIOD: u64 = 300_000;
pub const COUNTDOWN: u64 = 5_000;
pub const FINISH_WINDOW: u64 = 30_000;
pub const DISCONNECT_GRACE: u64 = 10_000;
pub const RESULTS_TTL: u64 = 600_000;
pub const MAX_ENTRANTS: usize = 16;
const FRESH: u64 = 2_000;

/// Assigned by the hub, never accepted from a command's payload. Local co-op
/// seats have distinct IDs even when they share an owner and career bank.
pub type Seat = u64;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Enrolling,
    Countdown,
    Running,
    Finishing,
    Complete,
    Cancelled,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    Joined,
    Racing,
    Finished,
    Left,
    Dnf,
    Disqualified,
}
#[derive(Clone, Copy, Debug)]
pub struct Sample {
    pub at: u64,
    pub p: [f64; 3],
}
#[derive(Clone, Debug)]
pub struct Entrant {
    pub outcome: Outcome,
    pub place: Option<u8>,
    pub finished_at: Option<u64>,
    pub joined_at: u64,
    sample: Sample,
    disconnected_at: Option<u64>,
    armed: bool,
}
impl Entrant {
    pub fn award(&self) -> u32 {
        match (self.outcome, self.place) {
            (Outcome::Finished, Some(1)) => 300,
            (Outcome::Finished, Some(2)) => 200,
            (Outcome::Finished, Some(3)) => 100,
            _ => 0,
        }
    }
    fn active(&self) -> bool {
        matches!(self.outcome, Outcome::Joined | Outcome::Racing)
    }
}
#[derive(Clone, Copy, Debug)]
pub struct Finish {
    pub p: [f64; 3],
    pub radius: f64,
    pub height: f64,
}
impl Finish {
    fn contains(&self, p: [f64; 3]) -> bool {
        xy2(p, self.p) <= self.radius * self.radius && (p[2] - self.p[2]).abs() <= self.height
    }
    /// Intersect the segment with the cylinder in both dimensions at the same
    /// parameter, rather than accepting a nearby bridge or a skipped endpoint.
    fn crosses(&self, a: [f64; 3], b: [f64; 3]) -> bool {
        let d = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
        let q = [a[0] - self.p[0], a[1] - self.p[1], a[2] - self.p[2]];
        let aa = d[0] * d[0] + d[1] * d[1];
        let bb = q[0] * d[0] + q[1] * d[1];
        let cc = q[0] * q[0] + q[1] * q[1] - self.radius * self.radius;
        let (mut lo, mut hi) = (0.0_f64, 1.0_f64);
        if aa < 1e-12 {
            if cc > 0.0 {
                return false;
            }
        } else {
            let disc = bb * bb - aa * cc;
            if disc < 0.0 {
                return false;
            }
            lo = lo.max((-bb - disc.sqrt()) / aa);
            hi = hi.min((-bb + disc.sqrt()) / aa);
        }
        if d[2].abs() < 1e-12 {
            if q[2].abs() > self.height {
                return false;
            }
        } else {
            let t0 = (-self.height - q[2]) / d[2];
            let t1 = (self.height - q[2]) / d[2];
            lo = lo.max(t0.min(t1));
            hi = hi.min(t0.max(t1));
        }
        lo <= hi
    }
}
fn valid(p: [f64; 3]) -> bool {
    p.iter().all(|v| v.is_finite() && v.abs() <= 32767.0)
}
fn xy2(a: [f64; 3], b: [f64; 3]) -> f64 {
    (a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)
}
fn at_start(p: [f64; 3], start: [f64; 3]) -> bool {
    xy2(p, start) <= 40.0 * 40.0 && (p[2] - start[2]).abs() < 10.0
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum JoinError {
    Closed,
    Full,
    Unavailable,
    InvalidPosition,
}
#[derive(Debug)]
pub struct Race {
    pub id: u64,
    pub room: u32,
    pub meet: u32,
    pub phase: Phase,
    pub start: [f64; 3],
    pub go_at: u64,
    pub close_at: u64,
    pub ended_at: Option<u64>,
    pub entrants: BTreeMap<Seat, Entrant>,
    finish: Finish,
    last_now: u64,
    next_place: u8,
}
impl Race {
    pub fn new(id: u64, room: u32, meet: u32, start: [f64; 3], go_at: u64, finish: Finish) -> Option<Self> {
        if !valid(start)
            || !valid(finish.p)
            || !finish.radius.is_finite()
            || !(1.0..=40.0).contains(&finish.radius)
            || !finish.height.is_finite()
            || !(0.5..=5.0).contains(&finish.height)
            || xy2(start, finish.p) < 100.0 * 100.0
            || !(COUNTDOWN..=u64::MAX - PERIOD - RESULTS_TTL).contains(&go_at)
        {
            return None;
        }
        Some(Self {
            id,
            room,
            meet,
            phase: Phase::Enrolling,
            start,
            go_at,
            close_at: go_at + PERIOD,
            ended_at: None,
            entrants: BTreeMap::new(),
            finish,
            last_now: 0,
            next_place: 1,
        })
    }
    /// A pre-start response must not reveal the destination, even to entrants.
    pub fn destination(&self) -> Option<Finish> {
        matches!(self.phase, Phase::Running | Phase::Finishing | Phase::Complete).then_some(self.finish)
    }
    pub fn join(&mut self, now: u64, seat: Seat, p: [f64; 3], compatible: impl Fn(Seat) -> bool) -> Result<(), JoinError> {
        if now < self.last_now {
            return Err(JoinError::Closed);
        }
        self.tick(now);
        if self.phase != Phase::Enrolling {
            return Err(JoinError::Closed);
        }
        if !valid(p) || !at_start(p, self.start) {
            return Err(JoinError::InvalidPosition);
        }
        if self.entrants.values().filter(|e| e.active()).count() >= MAX_ENTRANTS && !self.entrants.contains_key(&seat) {
            return Err(JoinError::Full);
        }
        if self.entrants.iter().any(|(&s, e)| s != seat && e.active() && !compatible(s)) {
            return Err(JoinError::Unavailable);
        }
        // Retried joins cannot reset disqualification, leave, timestamps or order.
        if let Some(e) = self.entrants.get(&seat) {
            return if e.outcome == Outcome::Joined { Ok(()) } else { Err(JoinError::Closed) };
        }
        if self.entrants.len() >= MAX_ENTRANTS {
            return Err(JoinError::Full);
        }
        self.entrants.insert(
            seat,
            Entrant {
                outcome: Outcome::Joined,
                place: None,
                finished_at: None,
                joined_at: now,
                sample: Sample { at: now, p },
                disconnected_at: None,
                armed: false,
            },
        );
        Ok(())
    }
    /// Voluntary leave while enrollment is open releases the slot and allows
    /// a new join. Policy withdrawals/disqualifications remain terminal.
    pub fn leave(&mut self, now: u64, seat: Seat) {
        if now < self.last_now {
            return;
        }
        self.tick(now);
        if self.phase == Phase::Enrolling && self.entrants.get(&seat).is_some_and(|e| e.outcome == Outcome::Joined) {
            self.entrants.remove(&seat);
        } else {
            self.withdraw(now, seat, false);
        }
    }
    pub fn withdraw(&mut self, now: u64, seat: Seat, disqualify: bool) {
        if now < self.last_now {
            return;
        }
        self.tick(now);
        if let Some(e) = self.entrants.get_mut(&seat)
            && e.active()
        {
            e.outcome = if disqualify { Outcome::Disqualified } else { Outcome::Left };
        }
        self.settle(now);
    }
    pub fn disconnect(&mut self, now: u64, seat: Seat) {
        if now < self.last_now {
            return;
        }
        self.tick(now);
        if let Some(e) = self.entrants.get_mut(&seat)
            && e.active()
        {
            e.disconnected_at.get_or_insert(now);
            e.armed = false;
        }
    }
    /// The hub must authenticate the original seat before calling this. A fresh
    /// first sample never proves a finish across the disconnected interval.
    pub fn reconnect(&mut self, now: u64, seat: Seat, p: [f64; 3]) -> bool {
        if now < self.last_now {
            return false;
        }
        self.tick(now);
        let Some(e) = self.entrants.get_mut(&seat) else {
            return false;
        };
        if !valid(p) || !e.active() || e.disconnected_at.is_none() {
            return false;
        }
        let distance = (xy2(e.sample.p, p) + (e.sample.p[2] - p[2]).powi(2)).sqrt();
        if distance > 125.0 * now.saturating_sub(e.sample.at) as f64 / 1000.0 + 10.0 {
            e.outcome = Outcome::Disqualified;
            self.settle(now);
            return false;
        }
        e.disconnected_at = None;
        e.sample = Sample { at: now, p };
        e.armed = false;
        true
    }
    pub fn sample(&mut self, now: u64, seat: Seat, p: [f64; 3], eligible: bool, discontinuity: bool) {
        if now < self.last_now {
            return;
        }
        self.tick(now);
        let Some(e) = self.entrants.get_mut(&seat) else {
            return;
        };
        if !e.active() || e.disconnected_at.is_some() || now <= e.sample.at {
            return;
        }
        if !valid(p) || discontinuity {
            e.outcome = Outcome::Disqualified;
            self.settle(now);
            return;
        }
        if !eligible {
            e.outcome = Outcome::Left;
            self.settle(now);
            return;
        }
        let old = e.sample;
        e.sample = Sample { at: now, p };
        if matches!(self.phase, Phase::Enrolling | Phase::Countdown) {
            if !at_start(p, self.start) {
                if self.phase == Phase::Enrolling {
                    // Leaving the meet before enrollment closes releases the
                    // place, but is not a permanent withdrawal. The hub keeps
                    // this race's invitation so the player can return and join.
                    self.entrants.remove(&seat);
                } else {
                    e.outcome = Outcome::Left;
                }
            }
            self.settle(now);
            return;
        }
        if !matches!(self.phase, Phase::Running | Phase::Finishing) {
            return;
        }
        let dt = now - old.at;
        let distance = (xy2(old.p, p) + (old.p[2] - p[2]).powi(2)).sqrt();
        // No interpolation over a stale interval. Plausibility is checked even
        // then, but packet loss must not turn a long segment into a finish.
        if distance > 125.0 * dt as f64 / 1000.0 + 10.0 {
            e.outcome = Outcome::Disqualified;
            self.settle(now);
            return;
        }
        if old.at >= self.go_at && dt <= FRESH && distance <= 260.0 && e.armed && self.finish.crosses(old.p, p) {
            e.outcome = Outcome::Finished;
            e.place = Some(self.next_place);
            e.finished_at = Some(now);
            self.next_place += 1;
            self.close_at = self.close_at.min(now.saturating_add(FINISH_WINDOW));
            self.phase = Phase::Finishing;
        } else {
            e.armed = !self.finish.contains(p);
        }
        self.settle(now);
    }
    pub fn tick(&mut self, now: u64) {
        if now < self.last_now || self.ended_at.is_some() {
            return;
        }
        self.last_now = now;
        for e in self.entrants.values_mut() {
            if e.active() && e.disconnected_at.is_some_and(|t| now.saturating_sub(t) >= DISCONNECT_GRACE) {
                e.outcome = Outcome::Dnf;
            }
        }
        if self.phase == Phase::Enrolling && now >= self.go_at - COUNTDOWN {
            self.revalidate(now);
            self.phase = Phase::Countdown;
            self.settle(now);
        }
        if self.phase == Phase::Countdown && now >= self.go_at {
            self.revalidate(now);
            self.settle(now);
            if self.phase == Phase::Countdown {
                self.phase = Phase::Running;
                for e in self.entrants.values_mut().filter(|e| e.active()) {
                    e.outcome = Outcome::Racing;
                    e.armed = false;
                }
            }
        }
        if matches!(self.phase, Phase::Running | Phase::Finishing) && now >= self.close_at {
            for e in self.entrants.values_mut().filter(|e| e.active()) {
                e.outcome = Outcome::Dnf;
            }
            self.settle(now);
        }
    }
    fn revalidate(&mut self, now: u64) {
        for e in self.entrants.values_mut().filter(|e| e.active()) {
            if e.disconnected_at.is_some() || now.saturating_sub(e.sample.at) > FRESH || !at_start(e.sample.p, self.start) {
                e.outcome = Outcome::Dnf;
            }
        }
    }
    fn settle(&mut self, now: u64) {
        if self.ended_at.is_some() {
            return;
        }
        let active = self.entrants.values().filter(|e| e.active()).count();
        if self.phase == Phase::Countdown && active < 2 {
            self.cancel(now);
        } else if matches!(self.phase, Phase::Running | Phase::Finishing) && active == 0 {
            self.phase = Phase::Complete;
            self.ended_at = Some(now);
        }
    }
    pub fn cancel(&mut self, now: u64) {
        if now < self.last_now || self.ended_at.is_some() {
            return;
        }
        for e in self.entrants.values_mut().filter(|e| e.active()) {
            e.outcome = Outcome::Dnf;
        }
        self.phase = Phase::Cancelled;
        self.ended_at = Some(now);
    }
}

/// Per-meet scheduling survives centroid movement and the departure of a racing
/// crowd. A disappearing enrolling meet gets 20 seconds to return. The hub
/// supplies a server-selected destination only when a new slot is constructed.
#[derive(Debug)]
pub struct Schedule {
    pub room: u32,
    pub meet: u32,
    pub start: [f64; 3],
    pub next_go: u64,
    pub last_seen: u64,
    pub race: Option<Race>,
}
impl Schedule {
    pub fn new(now: u64, room: u32, meet: u32, start: [f64; 3]) -> Option<Self> {
        if !valid(start) || now > u64::MAX - PERIOD - RESULTS_TTL {
            return None;
        }
        Some(Self { room, meet, start, next_go: now + PERIOD, last_seen: now, race: None })
    }
    pub fn seen(&mut self, now: u64) {
        self.last_seen = self.last_seen.max(now);
    }
    pub fn tick(&mut self, now: u64) {
        if let Some(r) = &mut self.race {
            // Observe disappearance before committing the final countdown.
            if r.phase == Phase::Enrolling && now.saturating_sub(self.last_seen) >= 20_000 {
                r.cancel(now);
            }
            r.tick(now);
        }
        if now >= self.next_go {
            let skipped = (now - self.next_go) / PERIOD + 1;
            self.next_go = self.next_go.saturating_add(skipped.saturating_mul(PERIOD));
        }
    }
    pub fn open(&mut self, now: u64, id: u64, finish: Finish) -> bool {
        self.tick(now);
        if now.saturating_sub(self.last_seen) >= 20_000
            || now >= self.next_go.saturating_sub(COUNTDOWN)
            || self.race.as_ref().is_some_and(|r| r.ended_at.is_none() || self.next_go <= r.go_at)
        {
            return false;
        }
        let Some(r) = Race::new(id, self.room, self.meet, self.start, self.next_go, finish) else {
            return false;
        };
        self.race = Some(r);
        true
    }
    pub fn expired(&self, now: u64) -> bool {
        now.saturating_sub(self.last_seen) >= RESULTS_TTL
            && self.race.as_ref().is_none_or(|r| r.ended_at.is_some_and(|t| now.saturating_sub(t) >= RESULTS_TTL))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn race() -> Race {
        Race::new(1, 1, 1, [0.0; 3], PERIOD, Finish { p: [1000.0, 0.0, 0.0], radius: 15.0, height: 3.0 }).unwrap()
    }
    fn running() -> Race {
        let mut r = race();
        for s in 1..=4 {
            r.join(PERIOD - 6000, s, [0.0; 3], |_| true).unwrap();
        }
        for t in [PERIOD - 5500, PERIOD - 4000, PERIOD - 2500, PERIOD - 1000, PERIOD] {
            for s in 1..=4 {
                r.sample(t, s, [0.0; 3], true, false);
            }
        }
        assert_eq!(r.phase, Phase::Running);
        r
    }
    fn finish(r: &mut Race, seat: Seat, offset: u64) {
        // Continuous 100 m/s travel, with a post-GO outside sample.
        for i in 1..=20 {
            r.sample(PERIOD + offset + i * 500, seat, [i as f64 * 50.0, 0.0, 0.0], true, false);
        }
    }
    #[test]
    fn roster_is_bounded_and_enrollment_leave_can_rejoin() {
        let mut r = race();
        for seat in 1..=MAX_ENTRANTS as u64 {
            r.join(1000, seat, r.start, |_| true).unwrap();
        }
        assert_eq!(r.join(1000, 99, r.start, |_| true), Err(JoinError::Full));
        r.leave(1100, 1);
        assert_eq!(r.join(1200, 1, r.start, |_| true), Ok(()));
        assert_eq!(r.join(1200, 99, r.start, |_| true), Err(JoinError::Full));
        assert_eq!(r.entrants.len(), MAX_ENTRANTS);
    }
    #[test]
    fn hidden_until_go_and_join_cutoff() {
        let mut r = race();
        assert!(r.destination().is_none());
        assert_eq!(r.join(PERIOD - COUNTDOWN, 1, [0.0; 3], |_| true), Err(JoinError::Closed));
        assert_eq!(r.phase, Phase::Cancelled);
        assert!(r.destination().is_none());
        assert!(running().destination().is_some());
    }
    #[test]
    fn podium_and_deadline_are_immutable() {
        let mut r = running();
        finish(&mut r, 1, 0);
        assert_eq!(r.entrants[&1].award(), 300);
        assert_eq!(r.close_at, PERIOD + 40_000);
        finish(&mut r, 2, 10_000);
        finish(&mut r, 3, 20_000);
        finish(&mut r, 4, 29_000);
        assert_eq!(r.phase, Phase::Complete);
        assert_eq!((r.entrants[&2].award(), r.entrants[&3].award(), r.entrants[&4].award()), (200, 100, 0));
        r.withdraw(PERIOD + 50_000, 1, true);
        r.cancel(PERIOD + 50_000);
        assert_eq!(r.entrants[&1].award(), 300);
        assert_eq!(r.phase, Phase::Complete);
    }
    #[test]
    fn timeout_never_promotes_unfinished_players() {
        let mut r = running();
        finish(&mut r, 1, 0);
        r.tick(PERIOD + 40_000);
        assert_eq!(r.phase, Phase::Complete);
        assert_eq!(r.entrants[&2].outcome, Outcome::Dnf);
        assert_eq!(r.entrants[&2].award(), 0);
        let mut r = running();
        r.tick(PERIOD * 2);
        assert_eq!(r.phase, Phase::Complete);
        assert!(r.entrants.values().all(|e| e.award() == 0));
    }
    #[test]
    fn last_remaining_must_finish() {
        let mut r = running();
        for s in 2..=4 {
            r.withdraw(PERIOD + 100, s, false);
        }
        assert_eq!(r.phase, Phase::Running);
        assert_eq!(r.entrants[&1].award(), 0);
        finish(&mut r, 1, 0);
        assert_eq!(r.phase, Phase::Complete);
        assert_eq!(r.entrants[&1].award(), 300);
    }
    #[test]
    fn blocks_and_split_seats() {
        let mut r = race();
        r.join(1, 1, [0.0; 3], |_| true).unwrap();
        assert_eq!(r.join(2, 2, [0.0; 3], |_| false), Err(JoinError::Unavailable));
        r.join(2, 2, [0.0; 3], |_| true).unwrap();
        assert_eq!(r.entrants.len(), 2);
        r.leave(3, 2);
        assert_eq!(r.join(4, 2, [0.0; 3], |_| false), Err(JoinError::Unavailable));
        assert_eq!(r.join(5, 2, [0.0; 3], |_| true), Ok(()));
        r.withdraw(6, 2, true);
        r.leave(7, 2);
        assert_eq!(r.join(8, 2, [0.0; 3], |_| true), Err(JoinError::Closed));
    }
    #[test]
    fn changing_activity_leaves_without_a_prize_or_disqualification() {
        let mut r = running();
        r.sample(PERIOD + 500, 1, [0.; 3], false, false);
        assert_eq!(r.entrants[&1].outcome, Outcome::Left);
        assert_eq!(r.entrants[&1].award(), 0);
        assert_eq!(r.phase, Phase::Running);
        r.sample(PERIOD + 1000, 1, [1000., 0., 0.], true, false);
        assert_eq!(r.entrants[&1].outcome, Outcome::Left);
    }
    #[test]
    fn invalid_movement_and_bridge_do_not_finish() {
        let mut r = running();
        r.sample(PERIOD + 500, 1, [1000.0, 0.0, 0.0], true, false);
        assert_eq!(r.entrants[&1].outcome, Outcome::Disqualified);
        for i in 1..=20 {
            r.sample(PERIOD + i * 500, 2, [i as f64 * 50.0, 0.0, 10.0], true, false);
        }
        assert_eq!(r.entrants[&2].award(), 0);
        r.sample(PERIOD + 11_000, 2, [1000.0, 0.0, 0.0], true, true);
        assert_eq!(r.entrants[&2].outcome, Outcome::Disqualified);
    }
    #[test]
    fn disconnect_grace_cannot_create_a_finish() {
        let mut r = running();
        r.disconnect(PERIOD + 100, 1);
        assert!(r.reconnect(PERIOD + 1000, 1, [100.0, 0.0, 0.0]));
        r.sample(PERIOD + 1500, 1, [150.0, 0.0, 0.0], true, false);
        assert_eq!(r.entrants[&1].award(), 0);
        r.disconnect(PERIOD + 1500, 1);
        r.tick(PERIOD + 11_500);
        assert!(!r.reconnect(PERIOD + 11_500, 1, [0.0; 3]));
        assert_eq!(r.entrants[&1].outcome, Outcome::Dnf);
    }
    #[test]
    fn segment_hits_at_consistent_height() {
        let f = Finish { p: [0.0; 3], radius: 10.0, height: 3.0 };
        assert!(f.crosses([-50.0, 0.0, 0.0], [50.0, 0.0, 0.0]));
        assert!(!f.crosses([-50.0, 0.0, 10.0], [50.0, 0.0, 10.0]));
        assert!(!f.crosses([-50.0, 0.0, 0.0], [50.0, 0.0, 30.0]));
    }
    #[test]
    fn schedule_does_not_restart_on_refresh_or_overlap() {
        let mut s = Schedule::new(0, 1, 1, [0.0; 3]).unwrap();
        let f = race().finish;
        assert!(s.open(0, 1, f));
        s.seen(290_000);
        s.tick(290_000);
        assert_eq!(s.next_go, PERIOD);
        assert_eq!(s.race.as_ref().unwrap().go_at, PERIOD);
        s.race = Some(running());
        s.tick(PERIOD);
        s.seen(PERIOD);
        assert_eq!(s.next_go, 2 * PERIOD);
        assert!(!s.open(PERIOD, 2, f));
        s.race.as_mut().unwrap().cancel(PERIOD + 1000);
        s.seen(PERIOD + 1000);
        assert!(s.open(PERIOD + 1000, 2, f));
        assert_eq!(s.race.as_ref().unwrap().go_at, 2 * PERIOD);
    }
    #[test]
    fn vanished_meet_cancels_enrollment_but_not_racing() {
        let mut s = Schedule::new(0, 1, 1, [0.0; 3]).unwrap();
        s.open(0, 1, race().finish);
        s.tick(19_999);
        assert_eq!(s.race.as_ref().unwrap().phase, Phase::Enrolling);
        s.tick(20_000);
        assert_eq!(s.race.as_ref().unwrap().phase, Phase::Cancelled);
        s.race = Some(running());
        s.tick(PERIOD + 20_000);
        assert_eq!(s.race.as_ref().unwrap().phase, Phase::Running);
        s.tick(2 * PERIOD);
        assert!(!s.expired(2 * PERIOD));
        assert!(s.expired(2 * PERIOD + RESULTS_TTL));
    }
    #[test]
    fn start_requires_two_fresh_cars_at_go_without_locking_them() {
        let mut r = race();
        for s in 1..=2 {
            r.join(PERIOD - 6000, s, [0.0; 3], |_| true).unwrap();
        }
        r.tick(PERIOD - 5000);
        assert_eq!(r.phase, Phase::Countdown);
        r.sample(PERIOD - 4000, 1, [39.0, 0.0, 0.0], true, false);
        assert_eq!(r.phase, Phase::Countdown);
        r.sample(PERIOD - 3500, 1, [41.0, 0.0, 0.0], true, false);
        assert_eq!(r.phase, Phase::Cancelled);
        let mut r = race();
        for s in 1..=2 {
            r.join(0, s, [0.0; 3], |_| true).unwrap();
        }
        r.tick(PERIOD);
        assert_eq!(r.phase, Phase::Cancelled);
    }
    #[test]
    fn replayed_or_late_samples_cannot_rewrite_outcomes() {
        let mut r = running();
        r.sample(PERIOD + 1000, 1, [100.0, 0.0, 0.0], true, false);
        r.sample(PERIOD + 500, 1, [1000.0, 0.0, 0.0], true, false);
        assert_eq!(r.entrants[&1].outcome, Outcome::Racing);
        r.withdraw(PERIOD + 1001, 1, false);
        r.sample(PERIOD + 2000, 1, [1000.0, 0.0, 0.0], true, false);
        assert_eq!(r.entrants[&1].outcome, Outcome::Left);
        assert_eq!(r.entrants[&1].award(), 0);
    }
    #[test]
    fn exact_finish_deadline_is_closed() {
        let mut r = running();
        finish(&mut r, 1, 0);
        r.sample(PERIOD + 39_000, 2, [900.0, 0.0, 0.0], true, false);
        r.sample(PERIOD + 40_000, 2, [1000.0, 0.0, 0.0], true, false);
        assert_eq!(r.entrants[&2].outcome, Outcome::Dnf);
    }
}
