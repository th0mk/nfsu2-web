//! Session-only speed trap. Scores are calculated from consecutive hub snapshots, not submitted scores.
//! Coordinates use the game's x/y ground plane and z height. This is a casual competition: snapshots
//! still originate on clients, so these plausibility checks are not authoritative physics or anti-cheat.

pub const CENTER: [f64; 3] = [1727.0, -776.0, 24.6];
#[derive(Clone, Debug, PartialEq)]
pub struct Trap {
    pub id: std::borrow::Cow<'static, str>,
    pub name: &'static str,
    pub ends: [[f64; 3]; 2],
}
impl Trap {
    pub fn center(&self) -> [f64; 3] {
        std::array::from_fn(|i| (self.ends[0][i] + self.ends[1][i]) * 0.5)
    }
    pub fn half_width(&self) -> f64 {
        (self.ends[1][0] - self.ends[0][0]).hypot(self.ends[1][1] - self.ends[0][1]) * 0.5
    }
}
// Airport road centreline surveyed with walls at + an overhead photo; 32.6 m across all lanes, normal to the road.
// Stable ids own scores. Add surveyed road locations here; the HUD selects the nearest automatically.
include!(concat!(env!("OUT_DIR"), "/speedtraps.rs"));


static CATALOG: std::sync::OnceLock<std::sync::RwLock<std::sync::Arc<Vec<Trap>>>> = std::sync::OnceLock::new();
pub fn current_catalog() -> std::sync::Arc<Vec<Trap>> {
    CATALOG.get_or_init(||std::sync::RwLock::new(std::sync::Arc::new(TRAPS.to_vec()))).read().unwrap().clone()
}
pub fn set_dev_catalog(gates: Vec<Trap>) {
    let lock = CATALOG.get_or_init(||std::sync::RwLock::new(std::sync::Arc::new(TRAPS.to_vec())));
    let mut current=lock.write().unwrap();
    if **current != gates { *current=std::sync::Arc::new(gates); }
}
pub fn parse_catalog(text: &str) -> Result<Vec<Trap>, &'static str> {
    let mut gates=Vec::new(); let mut ids=std::collections::HashSet::new();
    for line in text.lines().map(str::trim).filter(|s| !s.is_empty() && !s.starts_with("//")) {
        let body=line.strip_prefix("TRAP(").and_then(|s|s.strip_suffix(')')).ok_or("Invalid gate")?;
        let v:Vec<_>=body.split(',').map(str::trim).collect();
        if v.len()!=7 || v[0].is_empty() || v[0].len()>32 || !v[0].bytes().all(|c|c.is_ascii_lowercase()||c.is_ascii_digit()||c==b'_') || !ids.insert(v[0]) {return Err("Invalid id");}
        let xyz:Vec<f64>=v[1..].iter().map(|s|s.parse::<f64>().map_err(|_|"Invalid coordinate")).collect::<Result<_,_>>()?;
        if xyz.iter().any(|x|!x.is_finite()||x.abs()>=100000.) || !(2.0..=200.0).contains(&(xyz[3]-xyz[0]).hypot(xyz[4]-xyz[1])) {return Err("Invalid geometry");}
        gates.push(Trap{id:v[0].to_string().into(),name:"Speed trap",ends:[[xyz[0],xyz[1],xyz[2]],[xyz[3],xyz[4],xyz[5]]]});
        if gates.len()>64 {return Err("Too many gates");}
    }
    Ok(gates)
}

pub(crate) struct Traps {
    pub runs: Vec<Run>,
    pub catalog: std::sync::Arc<Vec<Trap>>,
    pub sequence: u32,
    pub requested_at: Option<f64>,
    pub event: Option<(usize, u32, bool)>,
}
impl Default for Traps {
    fn default() -> Self {
        let catalog = current_catalog();
        Self { runs: (0..catalog.len()).map(|_| Run::default()).collect(), catalog, sequence: 0, requested_at: None, event: None }
    }
}
impl Traps {
    pub fn interrupt(&mut self) {
        for r in &mut self.runs {
            r.interrupt();
        }
    }
    pub fn observe(&mut self, now: f64, p: [f64; 3], v: [f64; 3], tick: u32, clock: u8) {
        let catalog = current_catalog();
        if !std::sync::Arc::ptr_eq(&catalog, &self.catalog) {
            self.runs = (0..catalog.len()).map(|_|Run::default()).collect();
            self.catalog = catalog; self.event = None;
        }
        for (i, (r, gate)) in self.runs.iter_mut().zip(self.catalog.iter()).enumerate() {
            let (passes, best) = (r.passes, r.best);
            r.observe_gate(gate, now, p, v, tick, clock);
            if r.passes > passes {
                self.sequence = self.sequence.saturating_add(1);
                self.event = Some((i, r.last, r.best > best));
            }
        }
    }
}
pub const REQUEST: &[u8; 4] = b"STQ1";
pub const RESPONSE: &[u8; 4] = b"STP1";

#[derive(Clone, Copy)]
struct Sample {
    p: [f64; 3],
    v: [f64; 3],
    now: f64,
    tick: u32,
    clock: u8,
}

#[derive(Default)]
pub(crate) struct Run {
    sample: Option<Sample>,
    armed: bool,
    cooldown: f64,
    pub best: u32,
    pub last: u32,
    pub passes: u32,
}

impl Run {
    /// A pause, shop, race, reconnect or clock reset cannot bridge a crossing.
    pub fn interrupt(&mut self) {
        self.sample = None;
        self.armed = false;
    }

    #[cfg(test)]
    fn observe(&mut self, now: f64, p: [f64; 3], v: [f64; 3], tick: u32, clock: u8) {
        self.observe_gate(&TEST_GATE, now, p, v, tick, clock);
    }
    fn observe_gate(&mut self, gate: &Trap, now: f64, p: [f64; 3], v: [f64; 3], tick: u32, clock: u8) {
        let center = gate.center();
        let half_width = gate.half_width();
        if half_width < 1.0 {
            self.interrupt();
            return;
        }
        let tangent = [(gate.ends[1][0] - gate.ends[0][0]) / (half_width * 2.0), (gate.ends[1][1] - gate.ends[0][1]) / (half_width * 2.0)];
        let side = |p: [f64; 3]| -(p[0] - center[0]) * tangent[1] + (p[1] - center[1]) * tangent[0];
        if !now.is_finite() || !p.iter().chain(v.iter()).all(|x| x.is_finite()) || speed(v) > 140.0 {
            self.interrupt();
            return;
        }
        let current = Sample { p, v, now, tick, clock };
        let prev = self.sample.replace(current);
        let Some(a) = prev else {
            self.armed = side(p).abs() >= 25.0;
            return;
        };
        let dt = (now - a.now) / 1000.0;
        let game_dt = tick.wrapping_sub(a.tick) as f64 / 1_000_000.0;
        let moved = distance(a.p, p);
        // Clock jumps, duplicate/reordered packets, stalls, teleports and impossible displacement.
        if !(0.1..=2.0).contains(&dt)
            || !(0.1..=2.0).contains(&game_dt)
            || a.clock != clock
            || (game_dt - dt).abs() > 0.3 + dt * 0.25
            || moved > 12.0 + dt * (speed(a.v) + speed(v)) * 0.5
            || moved > 140.0 * dt + 5.0
        {
            self.interrupt();
            return;
        }
        let (before, after) = (side(a.p), side(p));
        if self.armed && now >= self.cooldown && ((before < 0.0 && after >= 0.0) || (before > 0.0 && after <= 0.0)) {
            let t = before / (before - after);
            let x = a.p[0] + t * (p[0] - a.p[0]);
            let y = a.p[1] + t * (p[1] - a.p[1]);
            let along = (x - center[0]) * tangent[0] + (y - center[1]) * tangent[1];
            let road_z = gate.ends[0][2] + (along / half_width + 1.0) * 0.5 * (gate.ends[1][2] - gate.ends[0][2]);
            let z = a.p[2] + t * (p[2] - a.p[2]);
            let velocity = std::array::from_fn(|i| a.v[i] + t * (v[i] - a.v[i]));
            let mps = speed(velocity);
            if along.abs() <= half_width && (-4.0..=45.0).contains(&(z - road_z)) && mps >= 5.0 {
                self.last = (mps * 36.0).round() as u32; // tenths of km/h
                self.best = self.best.max(self.last);
                self.passes = self.passes.saturating_add(1);
                self.cooldown = now + 5000.0;
                self.armed = false;
            }
        }
        if after.abs() >= 25.0 && now >= self.cooldown {
            self.armed = true;
        }
    }
}

fn speed(v: [f64; 3]) -> f64 {
    v[0].hypot(v[1])
}
fn distance(a: [f64; 3], b: [f64; 3]) -> f64 {
    ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) + (a[2] - b[2]).powi(2)).sqrt()
}

#[cfg(test)]
const TEST_GATE: Trap = Trap { id: std::borrow::Cow::Borrowed("airport"), name: "Test fixture", ends: [[1711., -773., 24.6], [1743., -779., 24.6]] };

#[cfg(test)]
mod tests {
    use super::*;
    fn sample(r: &mut Run, ms: f64, y: f64, speed: f64) {
        r.observe(ms, [CENTER[0], CENTER[1] + y, CENTER[2]], [0.0, speed, 0.0], (ms * 1000.0) as u32, 0);
    }
    #[test]
    fn gate_vertical_range_and_catalog_validation() {
        let gates=parse_catalog("TRAP(test,0,-20,10,0,20,10)").unwrap();
        for (z,expected) in [(6.,1),(10.,1),(55.,1),(55.1,0),(5.9,0)] {
            let mut r=Run::default();
            r.observe_gate(&gates[0],1000.,[-30.,0.,z],[60.,0.,0.],1_000_000,0);
            r.observe_gate(&gates[0],2000.,[30.,0.,z],[60.,0.,0.],2_000_000,0);
            assert_eq!(r.passes,expected,"height {z}");
        }
        assert!(parse_catalog("// empty catalog\n").unwrap().is_empty());
        assert!(parse_catalog("TRAP(a,0,0,0,1,0,0)").is_err());
        assert!(parse_catalog("TRAP(a,0,0,0,20,0,0)\nTRAP(a,0,0,0,20,0,0)").is_err());
    }
    #[test]
    fn fast_crossing_between_samples_scores_once_and_keeps_best() {
        let mut r = Run::default();
        sample(&mut r, 1000., -30., 60.);
        sample(&mut r, 2000., 30., 60.);
        assert_eq!((r.best, r.last, r.passes), (2160, 2160, 1));
        sample(&mut r, 2500., -1., -60.);
        sample(&mut r, 3000., 1., 60.);
        assert_eq!(r.passes, 1);
        r.interrupt();
        sample(&mut r, 8000., 30., -30.);
        sample(&mut r, 9500., -15., -30.);
        assert_eq!((r.best, r.last, r.passes), (2160, 1080, 2));
    }
    #[test]
    fn missing_samples_pause_and_teleport_do_not_score() {
        for (ms, y) in [(5000., 30.), (1500., 1000.), (1000., 30.)] {
            let mut r = Run::default();
            sample(&mut r, 1000., -30., 60.);
            sample(&mut r, ms, y, 60.);
            assert_eq!(r.passes, 0);
        }
        let mut r = Run::default();
        sample(&mut r, 1000., -30., 60.);
        r.interrupt();
        sample(&mut r, 2000., 30., 60.);
        assert_eq!(r.passes, 0);
    }
    #[test]
    fn another_road_or_clock_epoch_is_not_a_pass() {
        for (x, z, clock) in [(CENTER[0] + 20., CENTER[2], 0), (CENTER[0], CENTER[2] + 46., 0), (CENTER[0], CENTER[2], 1)] {
            let mut r = Run::default();
            r.observe(1000., [x, CENTER[1] - 30., z], [0., 60., 0.], 1_000_000, 0);
            r.observe(2000., [x, CENTER[1] + 30., z], [0., 60., 0.], 2_000_000, clock);
            assert_eq!(r.passes, 0);
        }
    }
    #[test]
    fn interpolates_velocity_and_accepts_wrapping_game_clock() {
        let mut r = Run::default();
        r.observe(1000., [CENTER[0], CENTER[1] - 30., CENTER[2]], [0., 40., 0.], u32::MAX - 499_999, 0);
        r.observe(2000., [CENTER[0], CENTER[1] + 30., CENTER[2]], [0., 80., 0.], 500_000, 0);
        assert_eq!(r.best, 2160);
    }
    #[test]
    fn edge_to_edge_gate_scores_every_lane_in_both_directions_but_not_beyond_the_road() {
        let gate = Trap { id: "test".into(), name: "Test road", ends: [[0., -30., 2.], [0., 30., 2.]] };
        for along in [-30., -20., 0., 20., 30.] {
            for direction in [-1., 1.] {
                let mut r = Run::default();
                r.observe_gate(&gate, 1000., [-30. * direction, along, 2.], [60. * direction, 0., 0.], 1_000_000, 0);
                r.observe_gate(&gate, 2000., [30. * direction, along, 2.], [60. * direction, 0., 0.], 2_000_000, 0);
                assert_eq!(r.passes, 1, "lane {along}, direction {direction}");
            }
        }
        for (along, z) in [(-31., 2.), (31., 2.), (0., 48.)] {
            let mut r = Run::default();
            r.observe_gate(&gate, 1000., [-30., along, z], [60., 0., 0.], 1_000_000, 0);
            r.observe_gate(&gate, 2000., [30., along, z], [60., 0., 0.], 2_000_000, 0);
            assert_eq!(r.passes, 0);
        }
    }
    #[test]
    fn rotated_sloping_gate_and_independent_trap_scores() {
        let a = Trap { id: "a".into(), name: "A", ends: [[-20., -20., 0.], [20., 20., 4.]] };
        let b = Trap { id: "b".into(), name: "B", ends: [[80., -20., 0.], [120., 20., 4.]] };
        let mut ra = Run::default();
        let mut rb = Run::default();
        for (ms, p) in [(1000., [-30., 30., 2.]), (2000., [30., -30., 2.])] {
            ra.observe_gate(&a, ms, p, [60., -60., 0.], (ms * 1000.) as u32, 0);
            rb.observe_gate(&b, ms, p, [60., -60., 0.], (ms * 1000.) as u32, 0);
        }
        assert_eq!(ra.passes, 1);
        assert_eq!(rb.best, 0);
    }
}
