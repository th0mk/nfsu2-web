//! The cheat watch: how a player's car moves, by its SNAPs to the hub (two a second, each with where its car is and
//! the time on the player's own clock), and what of it no car of the game does, as `[cheat]` lines (grep it).  The
//! address in a line is what `nfsu2-server ctl kick|ban` takes.
//!
//! - a teleport: the car went farther than DRIVE_MPS (450 km/h, more than any car of the game makes) takes it, by
//!   JUMP_M (300 m) or more.  What a car goes past that between two SNAPs is added up, and what it goes less, and
//!   LEAK_MPS (50 m) a second, takes off again, down to nothing: one jump across the map, and many short ones within a
//!   few seconds, both count; two of the Respawn button's jumps do not, however soon after each other.  A cheat
//!   that writes the car's place (a memory editor, the browser's developer tools) is one.  The game's own jumps are
//!   not: the Respawn button puts the car back on the road near it (well under JUMP_M); the World Map's jump moves the
//!   car within free roam, with no loading screen, onto an event's trigger or beside a shop's (15 m beside most, 39 m
//!   beside one), and a jump that lands within EVENT_JUMP_M of the one or SHOP_JUMP_M of the other (jumps.rs) is left
//!   be (nfsu2.xyz, 2026-10-28: some 95 players' lines in an hour, all of them such jumps, before it was; 10-29, two
//!   lines of that one shop's); a shop's or the garage's menus and a race go through a loading screen, a
//!   status other than free roam (2, or 5 paused there), which starts the watch afresh.  Nothing else does: a long
//!   silence lets the car go as far as DRIVE_MPS takes it in that time, and no farther.
//! - too fast: DRIVE_MPS or more between SNAPs for FAST_MS (2 s) on end.
//! - a fast clock: the player's clock went CLOCK_RATE (1.3) times as fast as the server's, over two counts of CLOCK_MS
//!   (30 s) on end.  A speed hack that makes the browser's time run faster makes the whole game faster, its car too.
//!   A count measures how far the player's clock got ahead of the server's past where its least late SNAP before
//!   the count had it (a SNAP's lateness: when it came by the server's clock, less its own time): a connection that
//!   held SNAPs back and then caught up gains back no more than their lateness (nfsu2.xyz, 2026-10-29: a player whose
//!   connection dropped every few minutes had a minute of a clock 1.4 times as fast, before it was), and a speed hack
//!   goes on past it.  The game's own clock keeps within 1 s of the browser's, and a new clock epoch starts the counts
//!   again, from its first SNAP: a connection that held that one back 9 s or more makes the first count look fast,
//!   and the next not.
//!
//! Bad connections are the players' lot, and no line comes of one.  The time between two SNAPs is the server's (when
//! they came), or the player's (their own times, in one clock epoch) where that is longer: a network that held SNAPs
//! back and then sent them at once moves no car faster.  A SNAP older than the last one (by its own time, or of an
//! older clock epoch) is left out: a page whose connection stalled opens another, and its SNAPs through the new one
//! can come before the old one's held-back ones (a car that went back and forth 550 m in a second, on a test's bad
//! connection, 2026-10-28).  A player gone from the hub without its BYE (its connection silent or dropped) keeps its
//! watch for a minute (hub.rs WATCH_KEEP_MS): back in that time, it goes on with it.  Nothing looks at the height
//! alone: a car that flies (an edited save) is left be unless it teleports or goes too fast.
//!
//! Each line of a teleport or a fast run says who was near where the car came to (the hub finds them: the other
//! players of its room in free roam within NEAR_M, 40 m, each where its own last SNAP and speed have it now), and
//! whether a race lobby starts there: the cheats that bother others are the ones that land on them or in their races.
//! A player gets LINES (5) lines, then one every LINE_BACK_MS (2 min); what no line told is counted, and summed up
//! every SUM_MS (10 min) and when the player leaves free roam's hub (at its BYE, or a minute after it went without
//! one): one line, the counts since it came.

use nfsu2_util::vip_str;

use crate::jumps;

/// what a car of the game drives at most, with room to spare (m/s: 450 km/h; the fastest tuned cars make about 350)
pub(crate) const DRIVE_MPS: f64 = 125.0;
/// a teleport goes this much farther than DRIVE_MPS takes a car (m): the Respawn button's jump stays well under it;
/// what a car went past DRIVE_MPS goes down by LEAK_MPS a second too (m/s)
pub(crate) const JUMP_M: f64 = 300.0;
const LEAK_MPS: f64 = 50.0;
/// a teleport that lands this near an event's trigger, or a shop's, is the World Map's jump (m): it lands on an
/// event's (20 m off once, of some 170 on nfsu2.xyz), 15 m beside most shops', 39 m beside one's
const EVENT_JUMP_M: f64 = 25.0;
const SHOP_JUMP_M: f64 = 45.0;
/// a run over DRIVE_MPS this long (ms) is told
const FAST_MS: f64 = 2000.0;
/// the player's clock this many times as fast as the server's, over CLOCK_MS at least (ms), is told
const CLOCK_RATE: f64 = 1.3;
const CLOCK_MS: f64 = 30_000.0;
/// the other players this near where the car came to (m) are next to it
pub(crate) const NEAR_M: f64 = 40.0;
/// the lines a player gets at once, and how long one takes to come back (ms)
const LINES: f64 = 5.0;
const LINE_BACK_MS: f64 = 120_000.0;
/// what no line told is summed up this often (ms)
const SUM_MS: f64 = 600_000.0;

/// one SNAP: where the car was, when the SNAP came (the hub's clock, ms), its time on the player's clock (µs, wraps)
/// and that clock's epoch
#[derive(Clone, Copy)]
struct Fix {
    p: [f64; 3],
    at: f64,
    t_us: u32,
    clock: u8,
}

/// What a SNAP showed
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum Seen {
    /// a teleport: from where to where (m apart), in how long (ms), and how far past what a car drives in that time
    Jump { from: [f64; 3], to: [f64; 3], d: f64, ms: f64, over: f64 },
    /// a run over DRIVE_MPS: from where to where, its speed (m/s, on average), how long so far (ms)
    Fast { from: [f64; 3], to: [f64; 3], mps: f64, ms: f64 },
    /// the player's clock ran `rate` times as fast as the server's over `ms`
    Clock { rate: f64, ms: f64 },
}

/// One player's watch
pub(crate) struct Watch {
    last: Option<Fix>,
    /// how far the car went past what DRIVE_MPS takes it (m), since where and for how long (ms)
    over: f64,
    over_from: [f64; 3],
    over_ms: f64,
    /// the run over DRIVE_MPS going on: since where, how long (ms), how far (m), and whether it was told
    run_from: [f64; 3],
    run_ms: f64,
    run_m: f64,
    run_told: bool,
    /// the clock's counts: the player's own time since its epoch's first SNAP (ms); since when this count goes (the
    /// hub's clock), and the least lateness (`clock`) of its SNAPs so far, and of all before it
    own_ms: f64,
    count_at: f64,
    count_low: f64,
    low: f64,
    /// the last count's rate and time (ms) when it was fast, and whether it was told
    clock_prev: Option<(f64, f64, bool)>,
    /// since it came: teleports (and of them next to another player), runs told, the time in them (ms) and their
    /// highest speed (m/s), the time its clock ran fast (ms) and how fast at most
    pub(crate) jumps: u32,
    near: u32,
    runs: u32,
    run_all_ms: f64,
    top: f64,
    clock_ms: f64,
    clock_max: f64,
    /// the lines it may still get (LINES at most), counted back from lines_at; what no line told since the last
    /// summary, and when that was; when it came
    lines: f64,
    lines_at: f64,
    quiet: u32,
    summed_at: f64,
    came: f64,
}

impl Watch {
    pub(crate) fn new(now: f64) -> Self {
        Watch {
            last: None,
            over: 0.0,
            over_from: [0.0; 3],
            over_ms: 0.0,
            run_from: [0.0; 3],
            run_ms: 0.0,
            run_m: 0.0,
            run_told: false,
            own_ms: 0.0,
            count_at: 0.0,
            count_low: 0.0,
            low: 0.0,
            clock_prev: None,
            jumps: 0,
            near: 0,
            runs: 0,
            run_all_ms: 0.0,
            top: 0.0,
            clock_ms: 0.0,
            clock_max: 0.0,
            lines: LINES,
            lines_at: now,
            quiet: 0,
            summed_at: now,
            came: now,
        }
    }

    /// out of free roam (a status other than 2 and 5): the next SNAP starts afresh
    pub(crate) fn reset(&mut self) {
        self.last = None;
        self.clock_prev = None;
        self.over = 0.0;
        self.end_run();
    }

    fn end_run(&mut self) {
        self.run_ms = 0.0;
        self.run_m = 0.0;
        self.run_told = false;
    }

    /// A SNAP in free roam (status 2 or 5): what it shows of the car's way since the last one (a teleport or a fast
    /// run), and of the player's clock; nothing of one older than the last
    pub(crate) fn snap(&mut self, now: f64, p: [f64; 3], t_us: u32, clock: u8) -> [Option<Seen>; 2] {
        let fix = Fix { p, at: now, t_us, clock };
        let Some(last) = self.last else {
            self.last = Some(fix);
            self.count_from(now);
            return [None, None];
        };
        // (epochs count on, mod 256, and a time on within one: both a 32-bit difference's sign)
        let same = clock == last.clock;
        let own = (t_us.wrapping_sub(last.t_us) as i32) as f64 / 1000.0;
        if (clock.wrapping_sub(last.clock) as i8) < 0 || same && own < 0.0 {
            return [None, None];
        }
        self.last = Some(fix);
        let recv = now - last.at;
        if same {
            self.own_ms += own;
        } else {
            self.count_from(now);
        }
        // (1 ms at least: two SNAPs at once with one time still move a car no farther than DRIVE_MPS takes it)
        let ms = recv.max(if same { own } else { 0.0 }).max(1.0);
        let d = distance(&last.p, &p);
        let past = d - DRIVE_MPS * ms / 1000.0;
        let gain = past - LEAK_MPS * ms / 1000.0;
        if self.over == 0.0 && gain > 0.0 {
            self.over_from = last.p;
            self.over_ms = 0.0;
        }
        self.over = (self.over + gain).max(0.0);
        self.over_ms += ms;
        let way = if self.over >= JUMP_M {
            let over = std::mem::take(&mut self.over);
            self.end_run();
            // (the World Map's jump: neither a line nor a count)
            (!map_jump(&p)).then(|| {
                self.jumps += 1;
                Seen::Jump { from: self.over_from, to: p, d: distance(&self.over_from, &p), ms: self.over_ms, over }
            })
        } else if past >= 0.0 {
            if self.run_ms == 0.0 {
                self.run_from = last.p;
            }
            self.run_ms += ms;
            self.run_m += d;
            let mps = self.run_m * 1000.0 / self.run_ms;
            if self.run_ms < FAST_MS {
                None
            } else {
                // (the whole run once it is one, then each SNAP of it)
                self.run_all_ms += if self.run_told { ms } else { self.run_ms };
                self.top = self.top.max(mps);
                (!std::mem::replace(&mut self.run_told, true)).then(|| {
                    self.runs += 1;
                    Seen::Fast { from: self.run_from, to: p, mps, ms: self.run_ms }
                })
            }
        } else {
            self.end_run();
            None
        };
        [way, if same { self.clock(now) } else { None }]
    }

    /// the clock's counts start again, from the first SNAP of a clock epoch (at `now`)
    fn count_from(&mut self, now: f64) {
        self.own_ms = 0.0;
        self.count_at = now;
        self.count_low = now;
        self.low = now;
        self.clock_prev = None;
    }

    /// The clock's count, at a SNAP (at `now`) of the same epoch as the last.  A SNAP's lateness is when it came less
    /// its own time since the epoch's first (both ms): a held-back SNAP's is more, and each SNAP of a clock that runs
    /// fast less than the one before.  Once CLOCK_MS went by in a count, how far its least lateness went under the one
    /// of all before it, for the time of the count, is how much faster the player's clock went; and the second fast
    /// count on end (and each one after it), how fast
    fn clock(&mut self, now: f64) -> Option<Seen> {
        let late = now - self.own_ms;
        self.count_low = self.count_low.min(late);
        let over = now - self.count_at;
        if over < CLOCK_MS {
            return None;
        }
        let rate = 1.0 + (self.low - self.count_low).max(0.0) / over;
        self.low = self.low.min(self.count_low);
        (self.count_at, self.count_low) = (now, late);
        let prev = std::mem::replace(&mut self.clock_prev, (rate >= CLOCK_RATE).then_some((rate, over, false)));
        let (was, was_ms, told) = prev.filter(|_| rate >= CLOCK_RATE)?;
        // (the first count of the run too, the first time)
        let (rate, ms) = (rate.min(was), if told { over } else { was_ms + over });
        self.clock_prev = Some((rate, over, true));
        self.clock_ms += ms;
        self.clock_max = self.clock_max.max(rate);
        Some(Seen::Clock { rate, ms })
    }

    /// a teleport of its landed next to another player
    pub(crate) fn landed_near(&mut self) {
        self.near += 1;
    }

    /// May a line tell what it did now?  If not, it is counted for the next summary.  Some(true): yes, and it is the
    /// last line for a while
    pub(crate) fn line(&mut self, now: f64) -> Option<bool> {
        self.lines = (self.lines + (now - self.lines_at) / LINE_BACK_MS).min(LINES);
        self.lines_at = now;
        if self.lines >= 1.0 {
            self.lines -= 1.0;
            Some(self.lines < 1.0)
        } else {
            self.quiet += 1;
            None
        }
    }

    /// what it did since it came, as a line: every SUM_MS while something was not told, and (`left`) when it leaves
    /// after anything at all
    pub(crate) fn summary(&mut self, vip: u32, name: &str, now: f64, left: bool) -> Option<String> {
        let due = if left { self.jumps + self.runs > 0 || self.clock_ms > 0.0 } else { self.quiet > 0 && now - self.summed_at >= SUM_MS };
        if !due {
            return None;
        }
        self.quiet = 0;
        self.summed_at = now;
        let mut parts = Vec::new();
        if self.jumps > 0 {
            let near = if self.near > 0 { format!(" ({} next to other players)", self.near) } else { String::new() };
            parts.push(format!("{} teleport{}{near}", self.jumps, plural(self.jumps)));
        }
        if self.runs > 0 {
            parts.push(format!(
                "{} run{} over {} for {} (at most {})",
                self.runs,
                plural(self.runs),
                kmh(DRIVE_MPS),
                span(self.run_all_ms),
                kmh(self.top)
            ));
        }
        if self.clock_ms > 0.0 {
            parts.push(format!("its clock fast for {} (up to {:.1} times the server's)", span(self.clock_ms), self.clock_max));
        }
        Some(format!(
            "[cheat] {} \"{name}\", {} in free roam ({}): {}",
            vip_str(vip),
            span(now - self.came),
            if left { "left" } else { "so far" },
            parts.join(", ")
        ))
    }
}

/// what was near where a car came to: the other players (how far, name, address; the nearest few) and how many in
/// all, and the host of a race lobby that starts there
pub(crate) struct Near<'a> {
    pub players: &'a [(f64, String, u32)],
    pub all: usize,
    pub lobby: Option<&'a str>,
}

/// a line of what a player's SNAP showed (`jumps`: its teleports so far); `last`: no more lines for a while
pub(crate) fn line(vip: u32, name: &str, seen: &Seen, near: &Near, jumps: u32, last: bool) -> String {
    let what = match *seen {
        Seen::Jump { from, to, d, ms, over } => format!(
            "teleport {jumps}: {} in {:.1} s, {} more than a car drives, {} to {}",
            metres(d),
            ms / 1000.0,
            metres(over),
            place(&from),
            place(&to)
        ),
        Seen::Fast { from, to, mps, ms } => format!("{} for {:.1} s, {} to {}", kmh(mps), ms / 1000.0, place(&from), place(&to)),
        Seen::Clock { rate, ms } => format!("its game's clock ran {rate:.1} times as fast as the server's for {} (a speed hack)", span(ms)),
    };
    let mut s = format!("[cheat] {} \"{name}\": {what}", vip_str(vip));
    if !near.players.is_empty() {
        let names: Vec<String> =
            near.players.iter().map(|(d, n, v)| format!("\"{n}\" {} ({})", vip_str(*v), metres(*d))).collect();
        let more = near.all - near.players.len();
        let more = if more > 0 { format!(" and {more} more") } else { String::new() };
        s += &format!("; next to {}{more}", names.join(", "));
    }
    if let Some(host) = near.lobby {
        s += &format!("; at the start of \"{host}\"'s race lobby");
    }
    if last {
        s += "; more are counted (summed up every 10 min and when it leaves)";
    }
    s
}

pub(crate) fn distance(a: &[f64; 3], b: &[f64; 3]) -> f64 {
    let (x, y, z) = (a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    (x * x + y * y + z * z).sqrt()
}

/// a car there came by the World Map's jump: near an event's or a shop's trigger (jumps.rs)
fn map_jump(p: &[f64; 3]) -> bool {
    let near = |&(x, y): &(f64, f64), m: f64| (p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y) < m * m;
    jumps::EVENTS.iter().any(|t| near(t, EVENT_JUMP_M)) || jumps::SHOPS.iter().any(|t| near(t, SHOP_JUMP_M))
}

fn plural(n: u32) -> &'static str {
    if n == 1 { "" } else { "s" }
}

fn kmh(mps: f64) -> String {
    format!("{:.0} km/h", mps * 3.6)
}

fn metres(m: f64) -> String {
    if m < 1000.0 { format!("{m:.0} m") } else { format!("{:.1} km", m / 1000.0) }
}

/// a place in whole metres, (x, y, height)
fn place(p: &[f64; 3]) -> String {
    format!("({:.0}, {:.0}, {:.0})", p[0], p[1], p[2])
}

fn span(ms: f64) -> String {
    let s = ms / 1000.0;
    if s < 90.0 { format!("{s:.0} s") } else if s < 5400.0 { format!("{:.0} min", s / 60.0) } else { format!("{:.1} h", s / 3600.0) }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// a player driving: a SNAP every 500 ms, its clock with the server's
    struct Drive {
        w: Watch,
        now: f64,
        t_us: u32,
        clock: u8,
    }

    impl Drive {
        fn new() -> Self {
            Drive { w: Watch::new(0.0), now: 0.0, t_us: 7_000_000, clock: 0 }
        }
        /// the next SNAP, `ms` later by the server's clock and `own` ms later by the player's
        fn at(&mut self, ms: f64, own: f64, p: [f64; 3]) -> [Option<Seen>; 2] {
            self.now += ms;
            self.t_us = self.t_us.wrapping_add((own * 1000.0) as u32);
            self.w.snap(self.now, p, self.t_us, self.clock)
        }
        fn snap(&mut self, p: [f64; 3]) -> [Option<Seen>; 2] {
            self.at(500.0, 500.0, p)
        }
        /// SNAPs from x0 to x1 at `mps`, one every 500 ms; what they showed
        fn drive(&mut self, x0: f64, x1: f64, mps: f64) -> Vec<Seen> {
            let n = ((x1 - x0) / (mps / 2.0)).round() as usize;
            (1..=n).flat_map(|i| self.snap([x0 + (x1 - x0) * i as f64 / n as f64, 0.0, 0.0])).flatten().collect()
        }
    }

    fn x(m: f64) -> [f64; 3] {
        [m, 0.0, 0.0]
    }

    // a car at 105 m/s (378 km/h, past the game's fastest) for 5 min is nothing; a jump of 2 km is a teleport, once,
    // and the SNAPs from there on are driving again
    #[test]
    fn a_teleport_is_seen_and_driving_is_not() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        assert_eq!(d.drive(0.0, 31_500.0, 105.0), []);
        let [jump, _] = d.snap(x(33_500.0));
        let Some(Seen::Jump { d: m, ms, over, .. }) = jump else { panic!("{jump:?}") };
        assert_eq!((m, ms, over), (2000.0, 500.0, 1912.5));
        assert_eq!(d.drive(33_500.0, 40_000.0, 100.0), []);
        assert_eq!(d.w.jumps, 1);
    }

    // a Respawn's jump (a few hundred metres at most past what the car drives) is not a teleport, nor a run, nor are
    // two of them a few seconds apart
    #[test]
    fn a_respawn_is_no_teleport() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        assert_eq!(d.snap(x(250.0)), [None, None]);
        assert_eq!(d.drive(250.0, 750.0, 100.0), []);
        assert_eq!(d.snap(x(1000.0)), [None, None]);
        assert_eq!((d.w.jumps, d.w.runs), (0, 0));
    }

    // short jumps, each under JUMP_M, a second apart: every second one adds up to a teleport
    #[test]
    fn short_jumps_add_up() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        let (mut at, mut seen) = (0.0, Vec::new());
        for _ in 0..4 {
            at += 290.0;
            seen.extend(d.snap(x(at)).into_iter().flatten());
            seen.extend(d.snap(x(at)).into_iter().flatten());
        }
        let [Seen::Jump { d: m, ms, .. }, Seen::Jump { from, .. }] = seen[..] else { panic!("{seen:?}") };
        assert_eq!((m, ms), (580.0, 1500.0), "the second jump, 1.5 s after the first began");
        assert_eq!(from, x(580.0), "then the third and fourth");
    }

    // the World Map's jump, 1-2 km onto an event's trigger or beside a shop's (three of nfsu2.xyz's, 2026-10-28 and
    // 10-29), is no teleport; 100 m off the trigger it is
    #[test]
    fn the_world_maps_jump_is_no_teleport() {
        let jump = |from: [f64; 3], to: [f64; 3]| {
            let mut d = Drive::new();
            d.snap(from);
            let [seen, _] = d.snap(to);
            (seen.is_some(), d.w.jumps)
        };
        assert_eq!(jump([-1261.0, 782.0, 48.0], [226.0, -478.0, 18.0]), (false, 0), "onto an event's trigger");
        assert_eq!(jump([-408.0, 22.0, 14.0], [352.0, -749.0, 14.0]), (false, 0), "beside the Performance Shop's");
        assert_eq!(jump([-71.0, -1584.0, 14.0], [-316.0, -458.0, 20.0]), (false, 0), "39 m beside a shop's (2026-10-29)");
        assert_eq!(jump([-1261.0, 782.0, 48.0], [326.0, -478.0, 18.0]), (true, 1), "100 m off the event's");
    }

    // a status out of free roam (a shop's or the garage's menus) between two SNAPs: no teleport
    #[test]
    fn out_of_free_roam_starts_afresh() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        d.w.reset();
        assert_eq!(d.snap(x(5000.0)), [None, None]);
        assert_eq!(d.w.jumps, 0);
    }

    // a new clock epoch, or a long silence, starts nothing afresh: the car went as far as the time takes it at most
    #[test]
    fn a_new_clock_or_silence_excuses_no_jump() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        d.clock = 1;
        assert!(matches!(d.snap(x(3000.0)), [Some(Seen::Jump { .. }), None]), "a new clock epoch");
        assert_eq!(d.at(20_000.0, 20_000.0, x(5000.0)), [None, None], "20 s later, 2 km on");
        assert!(matches!(d.at(20_000.0, 20_000.0, x(9000.0)), [Some(Seen::Jump { .. }), None]), "20 s later, 4 km on");
    }

    // two SNAPs held back by the network and sent at once: their own times say the car took 500 ms
    #[test]
    fn snaps_that_come_at_once_count_by_their_own_times() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        assert_eq!(d.at(1500.0, 500.0, x(50.0)), [None, None], "late");
        assert_eq!(d.at(1.0, 500.0, x(100.0)), [None, None], "1 ms after it");
        assert_eq!((d.w.jumps, d.w.run_ms), (0, 0.0));
    }

    // a page whose connection stalled opened another: the new one's SNAPs came first, then the old one's held back
    // (older by their own times): those are left out, and the car went nowhere it could not
    #[test]
    fn older_snaps_are_left_out() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        let t_old = d.t_us;
        // 6 s on at 100 m/s, by the new connection, then the old one's backlog of the 6 s
        assert_eq!(d.at(6500.0, 6000.0, x(600.0)), [None, None]);
        for i in 1..12 {
            let t = t_old.wrapping_add(i * 500_000);
            assert_eq!(d.w.snap(d.now + i as f64, x(i as f64 * 50.0), t, 0), [None, None], "old SNAP {i}");
        }
        assert_eq!(d.at(500.0, 500.0, x(650.0)), [None, None]);
        // an older clock epoch too
        d.clock = 5;
        d.snap(x(700.0));
        assert_eq!(d.w.snap(d.now + 1.0, x(9000.0), d.t_us, 4), [None, None]);
        assert_eq!(d.w.jumps, 0);
    }

    // 600 km/h: told once it lasted 2 s, once a run; the summary has it
    #[test]
    fn a_fast_run_is_told_once() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        let mut told = Vec::new();
        for i in 1..=10 {
            if let [Some(s), _] = d.snap(x(i as f64 * 83.3)) {
                told.push((i, s));
            }
        }
        assert_eq!(told.len(), 1, "{told:?}");
        let (i, Seen::Fast { mps, ms, .. }) = told[0] else { panic!() };
        assert_eq!((i, ms), (4, 2000.0));
        assert!((mps * 3.6 - 600.0).abs() < 1.0, "{mps}");
        d.snap(x(833.0));
        let s = d.w.summary(0x0A4D_0329, "Bob", d.now, true).unwrap();
        assert_eq!(s, "[cheat] 10.77.3.41 \"Bob\", 6 s in free roam (left): 1 run over 450 km/h for 5 s (at most 600 km/h)");
    }

    // a clock 3 times as fast: a count every 30 s, told from the second on
    #[test]
    fn a_fast_clock_is_told() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        let clocks: Vec<(usize, Seen)> =
            (1..=290).filter_map(|i| d.at(500.0, 1500.0, x(i as f64 * 50.0))[1].map(|c| (i, c))).collect();
        assert_eq!(clocks.iter().map(|c| c.0).collect::<Vec<_>>(), [120, 180, 240], "{clocks:?}");
        assert!(clocks.iter().all(|(_, c)| matches!(c, Seen::Clock { rate, .. } if (rate - 3.0).abs() < 0.2)), "{clocks:?}");
        assert!(matches!(clocks[0].1, Seen::Clock { ms: 60_000.0, .. }), "the two counts of 30 s: {:?}", clocks[0]);
    }

    // a connection that held every SNAP back 12 s for 10 s (the count's first 5 s among them), then caught up: the
    // first count looks fast (12 s gained in 30), and that is all
    #[test]
    fn a_connection_that_catches_up_is_no_fast_clock() {
        let mut d = Drive::new();
        d.now = 12_000.0;
        d.w.snap(d.now, x(0.0), d.t_us, 0);
        for i in 1..=20 {
            d.at(500.0, 500.0, x(i as f64 * 50.0));
        }
        // the backlog at once, then all in time
        for i in 21..=44 {
            d.at(1.0, 500.0, x(i as f64 * 50.0));
        }
        let mut clocks = Vec::new();
        let mut looked_fast = false;
        for i in 45..400 {
            clocks.extend(d.snap(x(i as f64 * 50.0))[1]);
            looked_fast |= d.w.clock_prev.is_some();
        }
        assert!(looked_fast, "a count looked fast");
        assert_eq!(clocks, []);
        assert_eq!(d.w.jumps, 0);
    }

    // a stall held a SNAP back 6 s, the ones the game sent then were dropped (a switch drops a burst past a browser's
    // share), and the next came 1 ms after it: the car drove 105 m/s all the while, and its own times say so (the
    // server's 1 ms would have made 630 m a teleport)
    #[test]
    fn a_stall_is_no_teleport() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        d.at(6500.0, 500.0, x(52.5));
        assert_eq!(d.at(1.0, 6000.0, x(682.5)), [None, None]);
        assert_eq!(d.drive(682.5, 2000.0, 105.0), []);
        assert_eq!(d.w.jumps, 0);
    }

    // an honest clock whose first SNAP came 12 s late, the next ones at once behind it, then all in time: the count
    // starts at one that was not late (from the late one, 30 s later its clock would have gone 1.4 times as fast)
    #[test]
    fn a_late_start_is_no_fast_clock() {
        let mut d = Drive::new();
        d.now = 12_000.0;
        d.w.snap(d.now, x(0.0), d.t_us, 0);
        for i in 1..=24 {
            d.at(1.0, 500.0, x(i as f64 * 50.0));
        }
        d.at(476.0, 500.0, x(25.0 * 50.0));
        let clocks: Vec<Seen> = (26..300).filter_map(|i| d.snap(x(i as f64 * 50.0))[1]).collect();
        assert_eq!(clocks, []);
        assert_eq!(d.w.jumps, 0);
    }

    // a slow upload, 5 min into free roam, that held the SNAPs back more and more for 2 min of the player's own time
    // (a minute behind at the end), then caught up in one (its clock twice as fast, by the server's): no line; a
    // speed hack of 1.4 after it is one (nfsu2.xyz, 2026-10-29: a player whose connection dropped every few minutes)
    #[test]
    fn a_backlog_that_drains_is_no_fast_clock() {
        let mut d = Drive::new();
        d.snap(x(0.0));
        let mut at = 0.0;
        let mut go = |d: &mut Drive, n: usize, ms: f64, own: f64| {
            let mut seen = Vec::new();
            for _ in 0..n {
                at += 50.0;
                seen.extend(d.at(ms, own, x(at)).into_iter().flatten());
            }
            seen
        };
        assert_eq!(go(&mut d, 600, 500.0, 500.0), []);
        assert_eq!(go(&mut d, 240, 750.0, 500.0), [], "falling behind");
        assert_eq!(go(&mut d, 240, 250.0, 500.0), [], "catching up");
        assert_eq!(go(&mut d, 600, 500.0, 500.0), []);
        let hack = go(&mut d, 240, 500.0, 700.0);
        assert!(matches!(hack[..], [Seen::Clock { rate, ms: 60_000.0 }, ..] if (rate - 1.4).abs() < 0.01), "{hack:?}");
    }

    // five lines at once, then one every 2 min; what is not told is summed up after 10 min
    #[test]
    fn lines_are_few() {
        let mut w = Watch::new(0.0);
        let got: Vec<Option<bool>> = (0..7).map(|_| w.line(0.0)).collect();
        assert_eq!(got, [Some(false), Some(false), Some(false), Some(false), Some(true), None, None]);
        assert_eq!(w.line(120_000.0), Some(true));
        w.jumps = 9;
        assert_eq!(w.summary(1, "Bob", 500_000.0, false), None, "not 10 min yet");
        assert!(w.summary(1, "Bob", 600_000.0, false).unwrap().ends_with("(so far): 9 teleports"));
        assert_eq!(w.summary(1, "Bob", 1_300_000.0, false), None, "all told since");
    }
}
