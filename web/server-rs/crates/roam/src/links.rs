//! The pieces the links are made of: the distance (Math.hypot as V8 computes it), the grid and its nearest-first walk
//! (Near), and the heap that merges the walks.  Together they give the pairs in the order a sort of every pair would,
//! bit for bit, ties included: the order decides between pairs at one distance, and so the links.

/// `Math.hypot(x, y, z)` as V8 computes it (scaled by the largest, Kahan's sum): the same distances as the Node hub,
/// so the same order of pairs.  (Rust never fuses the multiply and the subtraction into an FMA, nor does V8.)
pub(crate) fn hypot3(x: f64, y: f64, z: f64) -> f64 {
    let abs = [x.abs(), y.abs(), z.abs()];
    let mut max = 0.0f64;
    let mut nan = false;
    for &v in &abs {
        if v.is_nan() {
            nan = true;
        } else if v > max {
            max = v;
        }
    }
    if max == f64::INFINITY {
        return f64::INFINITY;
    }
    if nan {
        return f64::NAN;
    }
    if max == 0.0 {
        return 0.0;
    }
    let (mut sum, mut compensation) = (0.0f64, 0.0f64);
    for &v in &abs {
        let n = v / max;
        let summand = n * n - compensation;
        let preliminary = sum + summand;
        compensation = (preliminary - sum) - summand;
        sum = preliminary;
    }
    sum.sqrt() * max
}

/// the distance of two positions: `hypot3` of their differences in x, y and z, in that order
#[inline]
pub(crate) fn distance(a: &[f64; 3], b: &[f64; 3]) -> f64 {
    hypot3(a[0] - b[0], a[1] - b[1], a[2] - b[2])
}

/// A place as the links weigh it: x, y, the height, and how far up a player not in free roam counts (`prepare`:
/// RoamOptions::away_m, 0 in free roam).  The two stay apart: the height says whether two cars are on one road or on
/// two roads one above the other (`Level`), the "up" only puts a player in a shop farther off.
pub(crate) type Place = [f64; 4];

/// How the links weigh a gap in height that no road between two places explains.  A car on a highway and one on the
/// street under it are a few metres apart, and neither shows to the other; a link between them took a car's room from
/// one on the same road farther on.  A gap of up to `slack` metres, plus `grade` of the way across (a road up a hill,
/// a ramp, a car in the air over a bump), counts as it is, as it always did; each metre past that counts `k` metres
/// more.  So two cars on one road, however steep, weigh as before, and a car on a road above or below weighs as one
/// far off: linked when there is room, after the cars on its own road when there is not.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Level {
    pub(crate) slack: f64,
    pub(crate) grade: f64,
    pub(crate) k: f64,
    /// grade's square
    grade2: f64,
}

impl Default for Level {
    /// no gap weighs more than it is
    fn default() -> Level {
        Level::new(f64::INFINITY, 0.0, 0.0)
    }
}

impl Level {
    pub(crate) fn new(slack: f64, grade: f64, k: f64) -> Level {
        Level { slack, grade, k, grade2: grade * grade }
    }

    /// a gap in height `dz` as it counts, between two places `h2` apart across (the square of the distance in x and
    /// y); the same for (a, b) and (b, a), bit for bit.  Whether it is past what a road explains is found from the
    /// squares, both tests at once (one branch, nearly always the same way: a walk weighs every player of each cell it
    /// reads), and only a gap past it takes a root.
    #[inline]
    pub(crate) fn gap(&self, h2: f64, dz: f64) -> f64 {
        let v = dz.abs();
        let over = v - self.slack;
        if (over <= 0.0) | (over * over <= self.grade2 * h2) {
            return v;
        }
        v + self.k * (over - self.grade * h2.sqrt()).max(0.0)
    }

    /// the third side of a pair's distance: the gap in height as it counts and the difference in "up"
    #[inline]
    fn up(&self, a: &Place, b: &Place, h2: f64) -> f64 {
        self.gap(h2, a[2] - b[2]) + (a[3] - b[3]).abs()
    }
}

/// How far apart two places count for the links: `hypot3` of their differences in x and y and of `Level::up`.  Never
/// less than the distance in x and y (the grid's walk needs that); for two places with no gap past the slack and one
/// "up", the distance of before, bit for bit.
#[inline]
pub(crate) fn weigh(a: &Place, b: &Place, l: &Level) -> f64 {
    let (dx, dy, dz) = (a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    if one_level(a, b, dz, l) {
        return hypot3(dx, dy, dz);
    }
    hypot3(dx, dy, l.up(a, b, dx * dx + dy * dy))
}

/// the square of `weigh`, rounded (within 1e-15 of it, as the square of a distance always was)
#[inline]
pub(crate) fn square(a: &Place, b: &Place, l: &Level) -> f64 {
    let (dx, dy, dz) = (a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    let h2 = dx * dx + dy * dy;
    if one_level(a, b, dz, l) {
        return h2 + dz * dz;
    }
    let u = l.up(a, b, h2);
    h2 + u * u
}

/// Two places within the slack of each other in height and alike "up": nearly every pair a walk weighs (a crowd on
/// one spot is on one road), which takes the sums of before and nothing more (`Level::up` gives the same: `dz` as it
/// is).  One test for both (one branch, nearly always the same way).
#[inline]
fn one_level(a: &Place, b: &Place, dz: f64, l: &Level) -> bool {
    (dz.abs() <= l.slack) & (a[3] == b[3])
}

/// A number's place in the order of numbers as a u64 (the sign flipped, a negative one's bits all flipped; -0 as 0,
/// which it equals).
#[inline]
pub(crate) fn order_key(d: f64) -> u64 {
    let b = (d + 0.0).to_bits();
    b ^ (((b as i64) >> 63) as u64 | 1 << 63)
}

/// Sorts (key, index) by key, and at one key by the order they were in: the stable sort of a room's pairs by
/// their place.  A stable radix sort (11 bits a pass, a pass that would move nothing skipped): every pair of 250
/// players is 31,000 of them, which a comparison sort took most of a tick over.  `tmp`: room to work in.
pub(crate) fn sort_stable(v: &mut Vec<(u64, u32)>, tmp: &mut Vec<(u64, u32)>) {
    const BITS: u32 = 11;
    const BUCKETS: usize = 1 << BITS;
    let n = v.len();
    if n < 64 {
        v.sort_by_key(|e| e.0);
        return;
    }
    tmp.clear();
    tmp.resize(n, (0, 0));
    let mut count = vec![0usize; BUCKETS];
    let mut shift = 0;
    while shift < 64 {
        count.iter_mut().for_each(|c| *c = 0);
        for e in v.iter() {
            count[(e.0 >> shift) as usize & (BUCKETS - 1)] += 1;
        }
        // every key has this digit: nothing would move
        if count.contains(&n) {
            shift += BITS;
            continue;
        }
        let mut at = 0;
        for c in count.iter_mut() {
            let k = *c;
            *c = at;
            at += k;
        }
        for e in v.iter() {
            let b = (e.0 >> shift) as usize & (BUCKETS - 1);
            tmp[count[b]] = *e;
            count[b] += 1;
        }
        std::mem::swap(v, tmp);
        shift += BITS;
    }
}

/// the grid: cells (x and y) for about 8 players each, 5 m at least
const PER_CELL: f64 = 8.0;
const MIN_CELL: f64 = 5.0;

/// The players of a room (indices into its list) by their cell in x and y, sized for PER_CELL players over the area
/// they span, and the cells they span.  The cells are an array over the span (positions are within 20 km and cells at
/// least 5 m, so the span is at most 8001 cells a side and the array at most n/8 + 32004 cells); a cell outside the
/// span is empty.
#[derive(Default)]
pub(crate) struct Grid {
    pub(crate) cell: f64,
    pub(crate) x1: i64,
    pub(crate) x2: i64,
    pub(crate) y1: i64,
    pub(crate) y2: i64,
    rows: i64,
    /// where each cell's players start in `members` (one more than the cells: the end)
    start: Vec<u32>,
    members: Vec<u32>,
    /// each member's position, in the order of `members`: a walk reads a cell's players one after another in memory
    /// (by the list they are anywhere, and the reads of their positions were most of a ring's time)
    mpos: Vec<Place>,
    /// where each cell's players end (`remove` takes players out: a cell's players in `start[c]..end[c]`); and by
    /// index in the list, each one's place in `members` and its cell
    end: Vec<u32>,
    at: Vec<u32>,
    cell_of: Vec<u32>,
    /// how its walks weigh a gap in height (`weigh`)
    level: Level,
}

impl Grid {
    /// the grid of every position in `pos`, its walks weighing gaps in height as `level` says
    pub(crate) fn build(&mut self, pos: &[Place], level: Level) {
        self.level = level;
        self.start.clear();
        self.members.clear();
        if pos.is_empty() {
            *self = Grid { cell: MIN_CELL, x2: -1, y2: -1, ..std::mem::take(self) };
            return;
        }
        let (mut w1, mut w2, mut h1, mut h2) = (f64::INFINITY, f64::NEG_INFINITY, f64::INFINITY, f64::NEG_INFINITY);
        for p in pos {
            w1 = w1.min(p[0]);
            w2 = w2.max(p[0]);
            h1 = h1.min(p[1]);
            h2 = h2.max(p[1]);
        }
        let cell = MIN_CELL.max((1f64.max(w2 - w1) * 1f64.max(h2 - h1) * PER_CELL / pos.len() as f64).sqrt());
        self.cell = cell;
        self.x1 = (w1 / cell).floor() as i64;
        self.x2 = (w2 / cell).floor() as i64;
        self.y1 = (h1 / cell).floor() as i64;
        self.y2 = (h2 / cell).floor() as i64;
        self.rows = self.y2 - self.y1 + 1;
        let cells = ((self.x2 - self.x1 + 1) * self.rows) as usize;
        // counted, then placed (a counting sort): each cell's players in the order of the list
        self.start.clear();
        self.start.resize(cells + 1, 0);
        for p in pos {
            let c = self.index(p);
            self.start[c + 1] += 1;
        }
        for c in 0..cells {
            self.start[c + 1] += self.start[c];
        }
        self.members.clear();
        self.members.resize(pos.len(), 0);
        self.at.clear();
        self.at.resize(pos.len(), 0);
        self.cell_of.clear();
        for (i, p) in pos.iter().enumerate() {
            let c = self.index(p);
            // `start[c]` moves on as the cell fills: afterwards each start is the next cell's
            self.members[self.start[c] as usize] = i as u32;
            self.at[i] = self.start[c];
            self.cell_of.push(c as u32);
            self.start[c] += 1;
        }
        self.end.clear();
        self.end.extend_from_slice(&self.start[..cells]);
        for c in (1..=cells).rev() {
            self.start[c] = self.start[c - 1];
        }
        self.start[0] = 0;
        self.mpos.clear();
        self.mpos.extend(self.members.iter().map(|&i| pos[i as usize]));
    }

    /// Takes list[i] out: the walks read from then on pass it by (a player whose links are full, which no pair can link
    /// any more: `link_many`).  Its cell's last one takes its place; the order a cell's players are read in makes no
    /// difference to a walk.  Once out, it stays out until the next `build`.
    pub(crate) fn remove(&mut self, i: u32) {
        let (Some(&c), Some(&p)) = (self.cell_of.get(i as usize), self.at.get(i as usize)) else { return };
        let (c, p) = (c as usize, p as usize);
        let last = self.end[c] as usize;
        if p >= last {
            return;
        }
        let last = last - 1;
        let j = self.members[last];
        self.members.swap(p, last);
        self.mpos.swap(p, last);
        self.at[j as usize] = p as u32;
        self.at[i as usize] = last as u32;
        self.end[c] -= 1;
    }

    fn index(&self, p: &Place) -> usize {
        let (x, y) = ((p[0] / self.cell).floor() as i64, (p[1] / self.cell).floor() as i64);
        ((x - self.x1) * self.rows + (y - self.y1)) as usize
    }

    /// the players of cell (x, y) and their positions: none outside the span
    fn cell(&self, x: i64, y: i64) -> (&[u32], &[Place]) {
        if x < self.x1 || x > self.x2 || y < self.y1 || y > self.y2 {
            return (&[], &[]);
        }
        let c = ((x - self.x1) * self.rows + (y - self.y1)) as usize;
        let (a, z) = (self.start[c] as usize, self.end[c] as usize);
        (&self.members[a..z], &self.mpos[a..z])
    }
}

/// list[i]'s others in the grid, nearest first (`next()` gives (distance, j), or None after the last; at one
/// distance the lowest j first): ring after ring of cells around its own; one found is the nearest left once the
/// next ring is farther (a cell r rings away is at least r - 1 cells off).
///
/// A plain walk sorts what it found after each ring and takes the nearest off the end.  Here what
/// is found waits in a pool, in no order; once the ones within reach of the rings read (r - 1 cells) are all given,
/// the next ring is read and those within its reach move to `ready`, still in no order, and of those only the
/// nearest `CHUNK` at a time are put in order (`next`): of the 100 to 200 a player finds it takes 30 or 40 (in a
/// crowd on one spot, of 2000 within reach), so most are never sorted.  The same order, step for step: the nearest
/// found is in `next` or `ready` whenever any is within reach (every j is found once, so the order by distance,
/// then j, is total), and a ring is read exactly when a plain walk reads it, when none found is within reach.
///
/// Nor is the distance (Math.hypot as V8 computes it, three divisions) worked out for most.  The pool and `ready`
/// keep the square of the distance (rounded, within 1e-15 of the true one).  Where the reach decides, only a square
/// within 1e-9 of the reach's (a margin far wider than either rounding) needs the distance itself; where the
/// nearest CHUNK are taken, the distance of those and of the ones about as near (by the same margin), and of those
/// only the ones clearly nearer than the rest go into `next`.  A distance is never negative, so its bits are in its
/// order.
pub(crate) struct Near {
    i: u32,
    /// where the walk is from: list[i]'s position, or a point of its own (`from`)
    at: Place,
    /// how it weighs a gap in height (its grid's)
    level: Level,
    x0: i64,
    y0: i64,
    last: i64,
    r: i64,
    found: Found,
}

/// The ready ones put in order at a time (a player's links at most, 32: most take no more)
const CHUNK: usize = 32;
/// How far one square may be from another, relatively, and still be taken for the same distance: squares are
/// rounded by 1e-15 at most, Math.hypot by less; a square farther than this is a distance farther, for sure.
const MARGIN: f64 = 1e-9;

/// What a Near found and has not given yet.  Its vectors are kept from tick to tick.
#[derive(Default)]
pub(crate) struct Found {
    /// beyond the reach of the rings read so far, in no order: (the square of the distance, j)
    pool: Vec<(f64, u32)>,
    /// within it, in no order: (the square of the distance, j), each farther than every one in `next`
    ready: Vec<(f64, u32)>,
    /// the nearest of the ones within reach, in order, the nearest last: (the distance's bits, j)
    next: Vec<(u64, u32)>,
}

impl Near {
    /// `found`: empty vectors to keep them in (reused from tick to tick)
    pub(crate) fn new(g: &Grid, pos: &[Place], i: u32, found: Found) -> Near {
        Near::from(g, pos[i as usize], i, found)
    }

    /// the others of list[i] nearest first from point `at` (weigh(at, pos[j])), not from its own position
    pub(crate) fn from(g: &Grid, at: Place, i: u32, found: Found) -> Near {
        let (x0, y0) = ((at[0] / g.cell).floor() as i64, (at[1] / g.cell).floor() as i64);
        let last = (x0 - g.x1).max(g.x2 - x0).max(y0 - g.y1).max(g.y2 - y0);
        Near { i, at, level: g.level, x0, y0, last, r: 0, found }
    }

    /// its vectors back (emptied), for the next tick
    pub(crate) fn into_found(self) -> Found {
        let mut f = self.found;
        f.pool.clear();
        f.ready.clear();
        f.next.clear();
        f
    }

    pub(crate) fn next(&mut self, g: &Grid, pos: &[Place]) -> Option<(f64, u32)> {
        loop {
            if let Some((d, j)) = self.found.next.pop() {
                return Some((f64::from_bits(d), j));
            }
            if !self.found.ready.is_empty() {
                self.order(pos);
                continue;
            }
            // none found is within reach of the rings read (a plain walk's test of its nearest): the next ring, or
            // once every ring is read, all that is left
            let reach = if self.r <= self.last { self.r as f64 * g.cell } else { f64::INFINITY };
            if reach == f64::INFINITY && self.found.pool.is_empty() {
                return None;
            }
            let w = Within::new(reach);
            let (a, l) = (&self.at, &self.level);
            let Found { pool, ready, .. } = &mut self.found;
            pool.retain(|&(sq, j)| {
                if w.beyond(sq, || weigh(a, &pos[j as usize], l)) {
                    return true;
                }
                ready.push((sq, j));
                false
            });
            // the ring's own go where they belong as they are read
            if self.r <= self.last {
                self.ring(g, self.r, &w);
                self.r += 1;
            }
        }
    }

    /// The nearest CHUNK of the ready ones (or all, when they are few) into `next`, in order.  The CHUNK-th nearest
    /// by its square, s: every one whose square is under s (1 + MARGIN) has its distance worked out, and the ones
    /// within sqrt(s) (1 + MARGIN / 5) go: the CHUNK nearest by their squares are among them (their distances are at
    /// most sqrt(s) (1 + 2e-15)), and every one left is farther (a square over s (1 + MARGIN) is a distance over
    /// sqrt(s) (1 + MARGIN / 2.1)).
    fn order(&mut self, pos: &[Place]) {
        let (a, l) = (&self.at, &self.level);
        let Found { ready, next, .. } = &mut self.found;
        if ready.len() <= 2 * CHUNK {
            next.extend(ready.drain(..).map(|(_, j)| (weigh(a, &pos[j as usize], l).to_bits(), j)));
        } else {
            ready.select_nth_unstable_by(CHUNK - 1, |x, y| x.0.total_cmp(&y.0));
            let s = ready[CHUNK - 1].0;
            let (far, within) = (s * (1.0 + MARGIN), s.sqrt() * (1.0 + MARGIN / 5.0));
            ready.retain(|&(sq, j)| {
                if sq > far {
                    return true;
                }
                let d = weigh(a, &pos[j as usize], l);
                if d > within {
                    return true;
                }
                next.push((d.to_bits(), j));
                false
            });
        }
        next.sort_unstable_by(|x, y| y.cmp(x));
    }

    /// the cells r rings around its own (the order they are read in does not matter: what they give is ordered);
    /// the rows and columns outside the grid's span are skipped, as they hold nobody
    fn ring(&mut self, g: &Grid, r: i64, w: &Within) {
        let (x0, y0) = (self.x0, self.y0);
        if r == 0 {
            self.cell(g, x0, y0, w);
            return;
        }
        let (xa, xb) = ((x0 - r).max(g.x1), (x0 + r).min(g.x2));
        for y in [y0 - r, y0 + r] {
            if y >= g.y1 && y <= g.y2 {
                for x in xa..=xb {
                    self.cell(g, x, y, w);
                }
            }
        }
        let (ya, yb) = ((y0 - r + 1).max(g.y1), (y0 + r - 1).min(g.y2));
        for x in [x0 - r, x0 + r] {
            if x >= g.x1 && x <= g.x2 {
                for y in ya..=yb {
                    self.cell(g, x, y, w);
                }
            }
        }
    }

    /// a cell's players into the pool, or into `ready` when they are within reach of the ring being read
    fn cell(&mut self, g: &Grid, x: i64, y: i64, w: &Within) {
        let (a, l) = (&self.at, &self.level);
        let Found { pool, ready, .. } = &mut self.found;
        let (members, mpos) = g.cell(x, y);
        for (&j, b) in members.iter().zip(mpos) {
            if j != self.i {
                let sq = square(a, b, l);
                if w.beyond(sq, || weigh(a, b, l)) { pool.push((sq, j)) } else { ready.push((sq, j)) }
            }
        }
    }
}

/// Whether a square of a distance is a distance within `reach`: under `near` it is, over `far` it is not, between
/// the two the distance decides.
struct Within {
    reach: f64,
    near: f64,
    far: f64,
}

impl Within {
    fn new(reach: f64) -> Within {
        Within { reach, near: reach * reach * (1.0 - MARGIN), far: reach * reach * (1.0 + MARGIN) }
    }

    #[inline]
    fn beyond(&self, sq: f64, distance: impl FnOnce() -> f64) -> bool {
        sq > self.far || (sq >= self.near && distance() > self.reach)
    }
}

/// An entry of the order a large room's pairs are taken in: `(d, min(i, j), max(i, j), i, j, since, nearest)` in 16
/// bytes (a room of 2000 players has 34,000 of them).  `key` is `min << 34 | max << 4`, which walk found it (2 bits)
/// and two flags: whether this is i's nearest one (else a link of the last tick) and whether i is the higher one.
/// The order of `key >> 4` is the order of (min, max), and the walk and the flags take no part in it.  `since` is not
/// kept: it is the tick's time for a nearest one, and for a link of the last tick that link's (i's links do not change
/// in the tick).  Places in a room are below 2^30.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Cand {
    pub(crate) d: f64,
    key: u64,
}

const I_HIGH: u64 = 1;
const NEAREST: u64 = 2;
const MASK: u64 = (1 << 30) - 1;

/// the walks a player's nearest ones come from (`link_many`): by the places soon, by the places ahead, and from its
/// view point to the others' places soon
pub(crate) const WALK_SOON: u8 = 0;
pub(crate) const WALK_AHEAD: u8 = 1;
pub(crate) const WALK_VIEW: u8 = 2;

impl Cand {
    pub(crate) fn new(d: f64, i: u32, j: u32, nearest: bool) -> Cand {
        let (lo, hi) = (i.min(j) as u64 & MASK, i.max(j) as u64 & MASK);
        Cand { d, key: lo << 34 | hi << 4 | if nearest { NEAREST } else { 0 } | if i > j { I_HIGH } else { 0 } }
    }
    /// i's nearest one of walk `walk`
    pub(crate) fn by(d: f64, i: u32, j: u32, walk: u8) -> Cand {
        let mut c = Cand::new(d, i, j, true);
        c.key |= (walk as u64 & 3) << 2;
        c
    }
    fn lo(&self) -> u32 {
        (self.key >> 34) as u32
    }
    fn hi(&self) -> u32 {
        ((self.key >> 4) & MASK) as u32
    }
    pub(crate) fn i(&self) -> u32 {
        if self.key & I_HIGH != 0 { self.hi() } else { self.lo() }
    }
    pub(crate) fn j(&self) -> u32 {
        if self.key & I_HIGH != 0 { self.lo() } else { self.hi() }
    }
    pub(crate) fn nearest(&self) -> bool {
        self.key & NEAREST != 0
    }
    /// the walk of a nearest one
    pub(crate) fn walk(&self) -> u8 {
        (self.key >> 2 & 3) as u8
    }
    /// (min(i, j), max(i, j)) in one number, in their order
    pub(crate) fn pair(&self) -> u64 {
        self.key >> 4
    }
}

/// the least by d, then min(i, j), then max(i, j)
#[inline]
pub(crate) fn before(x: &Cand, y: &Cand) -> bool {
    x.d < y.d || (x.d == y.d && x.key >> 4 < y.key >> 4)
}

/// A binary heap, the least on top: each player's nearest one not taken yet (`link_many`, which says
/// why entries equal in (d, lo, hi) may come out in any order).
#[derive(Default)]
pub(crate) struct Heap {
    a: Vec<Cand>,
}

impl Heap {
    pub(crate) fn clear(&mut self) {
        self.a.clear();
    }

    pub(crate) fn peek(&self) -> Option<&Cand> {
        self.a.first()
    }

    pub(crate) fn push(&mut self, e: Cand) {
        let a = &mut self.a;
        a.push(e);
        let mut k = a.len() - 1;
        while k > 0 {
            let up = (k - 1) >> 1;
            if !before(&e, &a[up]) {
                break;
            }
            a[k] = a[up];
            k = up;
        }
        a[k] = e;
    }

    pub(crate) fn pop(&mut self) -> Option<Cand> {
        let a = &mut self.a;
        let top = *a.first()?;
        let e = a.pop()?;
        if !a.is_empty() {
            let mut k = 0;
            loop {
                let mut c = 2 * k + 1;
                if c >= a.len() {
                    break;
                }
                if c + 1 < a.len() && before(&a[c + 1], &a[c]) {
                    c += 1;
                }
                if !before(&a[c], &e) {
                    break;
                }
                a[k] = a[c];
                k = c;
            }
            a[k] = e;
        }
        Some(top)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hypot_as_v8() {
        assert_eq!(hypot3(3.0, 4.0, 0.0), 5.0);
        assert_eq!(hypot3(0.0, -0.0, 0.0), 0.0);
        assert!(hypot3(f64::NAN, 1.0, 0.0).is_nan());
        assert_eq!(hypot3(f64::NAN, f64::NEG_INFINITY, 0.0), f64::INFINITY);
    }

    #[test]
    fn sorts_as_a_stable_sort_by_the_number() {
        let mut seed = 1u64;
        let mut rnd = || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            seed >> 33
        };
        for n in [0, 1, 5, 63, 64, 65, 1000, 31125] {
            let ds: Vec<f64> = (0..n)
                .map(|_| match rnd() % 6 {
                    0 => -1.0,
                    1 => 0.0,
                    2 => -0.0,
                    3 => (rnd() % 50) as f64 * 0.5,
                    4 => rnd() as f64 * 1e-3,
                    _ => rnd() as f64 * 1e6,
                })
                .collect();
            let mut v: Vec<(u64, u32)> = ds.iter().enumerate().map(|(i, &d)| (order_key(d), i as u32)).collect();
            sort_stable(&mut v, &mut Vec::new());
            let mut want: Vec<u32> = (0..n as u32).collect();
            // a stable comparison sort by the number
            want.sort_by(|&x, &y| ds[x as usize].partial_cmp(&ds[y as usize]).unwrap());
            assert_eq!(v.iter().map(|e| e.1).collect::<Vec<_>>(), want, "{n}");
        }
    }

    #[test]
    fn heap_gives_the_least_first() {
        let mut h = Heap::default();
        for (d, i, j, nearest) in [(3.0, 1, 2, false), (1.0, 6, 5, true), (1.0, 2, 9, false), (-1.0, 7, 8, true), (1.0, 3, 2, false)] {
            h.push(Cand::new(d, i, j, nearest));
        }
        let order: Vec<(f64, u32, u32, bool)> = std::iter::from_fn(|| h.pop()).map(|e| (e.d, e.i(), e.j(), e.nearest())).collect();
        assert_eq!(order, [(-1.0, 7, 8, true), (1.0, 3, 2, false), (1.0, 2, 9, false), (1.0, 6, 5, true), (3.0, 1, 2, false)]);
        let big = Cand::new(0.5, (1 << 30) - 1, (1 << 30) - 2, true);
        assert_eq!((big.i(), big.j(), big.nearest(), big.walk()), ((1 << 30) - 1, (1 << 30) - 2, true, WALK_SOON));
        let a = Cand::by(0.5, 4, 9, WALK_AHEAD);
        assert_eq!((a.i(), a.j(), a.nearest(), a.walk(), a.pair()), (4, 9, true, WALK_AHEAD, Cand::new(0.5, 9, 4, false).pair()));
        let v = Cand::by(0.5, (1 << 30) - 1, 3, WALK_VIEW);
        assert_eq!((v.i(), v.j(), v.nearest(), v.walk()), ((1 << 30) - 1, 3, true, WALK_VIEW));
    }

    /// the hub's rule (RoamOptions level_*)
    const STEEP: Level = Level { slack: 5.0, grade: 0.2, k: 25.0, grade2: 0.2 * 0.2 };

    /// every other of list[i] (but i) from `from` by (weigh, j), as a sort of them all gives them
    fn sorted(pos: &[Place], i: u32, from: &Place, l: &Level) -> Vec<(f64, u32)> {
        let mut want: Vec<(f64, u32)> = (0..pos.len() as u32).filter(|&j| j != i).map(|j| (weigh(from, &pos[j as usize], l), j)).collect();
        want.sort_by(|x, y| x.0.total_cmp(&y.0).then(x.1.cmp(&y.1)));
        want
    }

    /// each one's walk gives the others as `sorted` does, bit for bit
    fn walks_as_sorted(pos: &[Place], l: Level) {
        let mut g = Grid::default();
        g.build(pos, l);
        for i in 0..pos.len() as u32 {
            let mut n = Near::new(&g, pos, i, Found::default());
            let got: Vec<(f64, u32)> = std::iter::from_fn(|| n.next(&g, pos)).collect();
            let want = sorted(pos, i, &pos[i as usize], &l);
            assert_eq!(got.len(), want.len(), "{} players, player {i}, {l:?}", pos.len());
            assert!(
                got.iter().zip(&want).all(|(a, b)| a.0.to_bits() == b.0.to_bits() && a.1 == b.1),
                "{} players, player {i}, {l:?}",
                pos.len()
            );
        }
    }

    /// a place in free roam (nothing "up")
    fn at(x: f64, y: f64, z: f64) -> Place {
        [x, y, z, 0.0]
    }

    #[test]
    fn near_walks_nearest_first() {
        let pos: Vec<Place> = (0..50).map(|i| at((i * 7 % 50) as f64 * 3.0, (i * 11 % 50) as f64 * 2.0, 0.0)).collect();
        for l in [Level::default(), STEEP] {
            walks_as_sorted(&pos, l);
        }
    }

    // the walk on lattices whose points sit on the cells' edges (distances of exactly the reach, 5 m cells), on a few
    // spots (players at one place), and on scattered float32 positions near and far: all others by (distance, j); and
    // the same with roads over roads (a lattice at 0 m and one 6 to 30 m over it; a highway over streets with a hill
    // and players in a shop), where gaps in height weigh more than they are
    #[test]
    fn near_walks_nearest_first_at_the_edges() {
        let mut seed = 7u64;
        let mut rnd = || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            (seed >> 11) as f64 / (1u64 << 53) as f64
        };
        let lattice = |n: usize, step: f64, z: f64| -> Vec<Place> {
            (0..n * n).map(|k| at((k % n) as f64 * step, (k / n) as f64 * step, z)).collect()
        };
        let mut layouts = vec![lattice(10, 1.0, 0.0), lattice(12, 5.0, 0.0), lattice(9, 2.5, 0.0)];
        layouts.push((0..120).map(|k| [at(0.0, 0.0, 0.0), at(5.0, 0.0, 0.0), at(0.0, 10.0, 1.0)][k % 3]).collect());
        layouts.push(
            (0..300)
                .map(|k| {
                    let r = if k % 10 == 0 { 15000.0 } else { 400.0 };
                    [(rnd() * 2.0 - 1.0) * r, (rnd() * 2.0 - 1.0) * r, rnd() * 20.0, 0.0].map(|x| x as f32 as f64)
                })
                .collect(),
        );
        for (n, step, up) in [(10, 5.0, 6.0), (12, 2.5, 10.0), (8, 5.0, 30.0)] {
            let mut two = lattice(n, step, 0.0);
            two.extend(lattice(n, step, up));
            layouts.push(two);
        }
        layouts.push(
            (0..400)
                .map(|k| {
                    let (x, y) = ((rnd() * 2.0 - 1.0) * 300.0, (rnd() * 2.0 - 1.0) * 300.0);
                    let z = match k % 4 {
                        0 => 12.0,
                        1 => 0.2 * x.abs(),
                        _ => rnd() * 2.0,
                    };
                    [x, y, z, if k % 9 == 0 { 100.0 } else { 0.0 }].map(|v| v as f32 as f64)
                })
                .collect(),
        );
        for pos in &layouts {
            for l in [Level::default(), STEEP] {
                walks_as_sorted(pos, l);
            }
        }
    }

    // from a point of its own (inside the players' span, on a cell's edge, far outside it, above them, in a shop): all
    // others but i by (weigh(from, pos[j]), j)
    #[test]
    fn near_walks_nearest_first_from_a_point() {
        let pos: Vec<Place> = (0..80).map(|i| at((i * 7 % 40) as f64 * 5.0, (i * 13 % 40) as f64 * 2.5, (i % 3) as f64 * 7.0)).collect();
        let froms = [
            (0u32, at(37.5, 20.0, 1.0)),
            (5, at(15.0, 10.0, 0.0)),
            (79, at(-400.0, 900.0, 100.0)),
            (12, at(190.0, -3.0, 0.0)),
            (3, [60.0, 40.0, 14.0, 100.0]),
        ];
        for l in [Level::default(), STEEP] {
            let mut g = Grid::default();
            g.build(&pos, l);
            for (i, from) in froms {
                let mut n = Near::from(&g, from, i, Found::default());
                let got: Vec<(f64, u32)> = std::iter::from_fn(|| n.next(&g, &pos)).collect();
                let want = sorted(&pos, i, &from, &l);
                assert_eq!(got.len(), want.len(), "from {from:?}");
                assert!(got.iter().zip(&want).all(|(a, b)| a.0.to_bits() == b.0.to_bits() && a.1 == b.1), "from {from:?}");
            }
        }
    }

    // two places with no gap past what a road explains, and the same "up", weigh what their distance always was, bit
    // for bit; any two weigh the same both ways, never less than their distance in x and y, and their square is the
    // weight's
    #[test]
    fn weighs_as_the_distance_on_one_road() {
        let mut seed = 3u64;
        let mut rnd = || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            (seed >> 11) as f64 / (1u64 << 53) as f64
        };
        for _ in 0..20000 {
            let up = |r: f64| if r < 0.2 { 100.0 } else { 0.0 };
            let a = [(rnd() - 0.5) * 6000.0, (rnd() - 0.5) * 6000.0, rnd() * 60.0, up(rnd())].map(|v| v as f32 as f64);
            let b = [a[0] + (rnd() - 0.5) * 400.0, a[1] + (rnd() - 0.5) * 400.0, a[2] + (rnd() - 0.5) * 40.0, up(rnd())]
                .map(|v| v as f32 as f64);
            for l in [Level::default(), STEEP] {
                let d = weigh(&a, &b, &l);
                let across = (a[0] - b[0]).hypot(a[1] - b[1]);
                assert_eq!(d.to_bits(), weigh(&b, &a, &l).to_bits(), "{a:?} {b:?}");
                assert_eq!(square(&a, &b, &l).to_bits(), square(&b, &a, &l).to_bits(), "{a:?} {b:?}");
                assert!(d >= across * (1.0 - 1e-15), "{a:?} {b:?}");
                assert!((square(&a, &b, &l) - d * d).abs() <= d * d * 1e-14, "{a:?} {b:?}");
                if (a[2] - b[2]).abs() <= l.slack + l.grade * across && a[3] == b[3] {
                    let (x, y) = ([a[0], a[1], a[2]], [b[0], b[1], b[2]]);
                    assert_eq!(d.to_bits(), distance(&x, &y).to_bits(), "{a:?} {b:?}");
                    let (dx, dy, dz) = (a[0] - b[0], a[1] - b[1], a[2] - b[2]);
                    assert_eq!(square(&a, &b, &l).to_bits(), (dx * dx + dy * dy + dz * dz).to_bits(), "{a:?} {b:?}");
                }
            }
        }
    }

    // what gaps weigh: a road up a hill as it is; the street under a highway (10 m below) as a car 135 m off, less
    // to the side, as it is once in sight; a car 4 m up over a bump beside ours as it is; a player in a shop 100 m up
    #[test]
    fn weighs_a_road_above_as_one_far_off() {
        let w = |a: Place, b: Place| (weigh(&a, &b, &STEEP) * 10.0).round() / 10.0;
        assert_eq!(w(at(0.0, 0.0, 0.0), at(100.0, 0.0, 20.0)), 102.0);
        assert_eq!(w(at(0.0, 0.0, 10.0), at(0.0, 0.0, 0.0)), 135.0);
        assert_eq!(w(at(0.0, 0.0, 10.0), at(8.0, 0.0, 0.0)), 95.3);
        assert_eq!(w(at(0.0, 0.0, 10.0), at(40.0, 0.0, 0.0)), 41.2);
        assert_eq!(w(at(0.0, 0.0, 4.0), at(3.0, 0.0, 0.0)), 5.0);
        assert_eq!(w([0.0, 0.0, 0.0, 100.0], at(0.0, 0.0, 0.0)), 100.0);
        assert_eq!(w([30.0, 0.0, 2.0, 100.0], [0.0, 0.0, 0.0, 100.0]), 30.1);
    }
}
