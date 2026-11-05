//! Server-owned road graph from the operator's game data. No client-supplied
//! coordinates or region labels are trusted. See runtime/walls.c for the native
//! road/junction layout and career-barrier intersection rule.
use std::cmp::Reverse;
use std::collections::{BinaryHeap, HashMap};

#[derive(Debug, Clone)]
pub struct Node {
    pub p: [f64; 3],
    pub region: u8,
}
#[derive(Debug)]
pub struct Roads {
    pub nodes: Vec<Node>,
    edges: Vec<(usize, usize, f64)>,
    adj: Vec<Vec<(usize, f64)>>,
    barriers: Vec<[f64; 4]>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InvalidData(pub &'static str);
type Result<T> = std::result::Result<T, InvalidData>;
fn u16_at(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes(b[o..o + 2].try_into().unwrap())
}
fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn f32_at(b: &[u8], o: usize) -> f64 {
    f32::from_le_bytes(b[o..o + 4].try_into().unwrap()) as f64
}
fn finite(p: &[f64]) -> bool {
    p.iter().all(|v| v.is_finite() && v.abs() <= 32767.0)
}
fn chunks(b: &[u8]) -> Result<Vec<(u32, &[u8])>> {
    if b.len() > 4_000_000 {
        return Err(InvalidData("oversized road data"));
    }
    let mut out = Vec::new();
    let mut at = 0;
    while at < b.len() {
        if at + 8 > b.len() || out.len() >= 4096 {
            return Err(InvalidData("invalid chunk header"));
        }
        let n = u32_at(b, at + 4) as usize;
        if n > b.len() - at - 8 {
            return Err(InvalidData("truncated chunk"));
        }
        out.push((u32_at(b, at), &b[at + 8..at + 8 + n]));
        at += 8 + n;
    }
    Ok(out)
}
fn unique<'a>(chunks: &[(u32, &'a [u8])], key: u32) -> Result<&'a [u8]> {
    let mut matches = chunks.iter().filter(|c| c.0 == key);
    let found = matches.next().ok_or(InvalidData("missing road chunk"))?.1;
    if matches.next().is_some() {
        return Err(InvalidData("duplicate road chunk"));
    }
    Ok(found)
}
fn distance(a: [f64; 3], b: [f64; 3]) -> f64 {
    ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) + (a[2] - b[2]).powi(2)).sqrt()
}
fn crosses(a: [f64; 3], b: [f64; 3], c: [f64; 4]) -> bool {
    let d1 = (c[2] - c[0]) * (a[1] - c[1]) - (c[3] - c[1]) * (a[0] - c[0]);
    let d2 = (c[2] - c[0]) * (b[1] - c[1]) - (c[3] - c[1]) * (b[0] - c[0]);
    let d3 = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    let d4 = (b[0] - a[0]) * (c[3] - a[1]) - (b[1] - a[1]) * (c[2] - a[0]);
    (d1 > 0.0) != (d2 > 0.0) && (d3 > 0.0) != (d4 > 0.0)
}
impl Roads {
    #[cfg(test)]
    pub(crate) fn test_empty() -> Self {
        Self { nodes: Vec::new(), edges: Vec::new(), adj: Vec::new(), barriers: Vec::new() }
    }

    /// RoutesFreeRoam.bin and PathsFreeRoam.bin from TRACKS/ROUTESL4RA.
    /// Native loaders: 0x5de1c0, 0x5deb80, 0x5d9a90, 0x5d5010.
    pub fn parse(routes: &[u8], paths: &[u8]) -> Result<Self> {
        let rc = chunks(routes)?;
        let raw = unique(&rc, 0x34121)?;
        let mut nodes = Vec::new();
        let mut roads = HashMap::new();
        let mut edges = Vec::new();
        let mut at = 0;
        while at < raw.len() {
            if at + 0x8c > raw.len() {
                return Err(InvalidData("truncated road"));
            }
            let id = u16_at(raw, at + 0xa);
            let n = u16_at(raw, at + 0x34) as usize;
            if id >= 2048 || !(2..=2048).contains(&n) || at + 0x8c + n * 0x38 > raw.len() || nodes.len() + n > 32768 {
                return Err(InvalidData("invalid road"));
            }
            let base = nodes.len();
            if roads.insert(id, (base, n)).is_some() {
                return Err(InvalidData("duplicate road"));
            }
            for i in 0..n {
                let o = at + 0x8c + i * 0x38;
                let p = [f32_at(raw, o), f32_at(raw, o + 4), (u16_at(raw, o + 0x12) as i16) as f64 / 8.0];
                if !finite(&p) {
                    return Err(InvalidData("invalid road coordinate"));
                }
                nodes.push(Node { p, region: 0 });
                if i > 0 {
                    edges.push((base + i - 1, base + i));
                }
            }
            at += 0x8c + n * 0x38;
        }
        if nodes.is_empty() {
            return Err(InvalidData("empty roads"));
        }
        let joints = unique(&rc, 0x34122)?;
        if joints.len() < 4 || (joints.len() - 4) % 0x20 != 0 || joints.len() > 4 + 65536 * 0x20 {
            return Err(InvalidData("invalid junctions"));
        }
        for j in joints[4..].chunks_exact(0x20) {
            let (n1, n2, r1, r2) = (u16_at(j, 4) as usize, u16_at(j, 6) as usize, u16_at(j, 8), u16_at(j, 10));
            let (Some(&(b1, c1)), Some(&(b2, c2))) = (roads.get(&r1), roads.get(&r2)) else {
                return Err(InvalidData("unknown junction road"));
            };
            if n1 >= c1 || n2 >= c2 {
                return Err(InvalidData("unknown junction node"));
            }
            edges.push((b1 + n1, b2 + n2));
        }
        let pc = chunks(paths)?;
        let pc = chunks(unique(&pc, 0x80034147)?)?;
        let raw = unique(&pc, 0x3414d)?;
        if raw.len() % 0x24 != 0 {
            return Err(InvalidData("invalid barriers"));
        }
        let mut barriers = Vec::new();
        for b in raw.chunks_exact(0x24) {
            let n = b[0x11] as usize;
            if n > 4 {
                return Err(InvalidData("invalid barrier groups"));
            }
            let mut mask = 0u8;
            for i in 0..n {
                // bStringHash("BARRIERS_CAREER0") + stage.
                let stage = u32_at(b, 0x14 + 4 * i).wrapping_sub(0xd59bf29a);
                if stage <= 5 {
                    mask |= 1 << stage;
                }
            }
            if mask != 0 {
                let p = [f32_at(b, 0), f32_at(b, 4), f32_at(b, 8), f32_at(b, 12)];
                if !finite(&p) {
                    return Err(InvalidData("invalid barrier coordinate"));
                }
                barriers.push((p, mask));
            }
        }
        if barriers.is_empty() {
            return Err(InvalidData("missing career barriers"));
        }
        let masks: Vec<u8> = edges
            .iter()
            .map(|&(a, b)| barriers.iter().filter(|(p, _)| crosses(nodes[a].p, nodes[b].p, *p)).fold(0, |v, (_, m)| v | m))
            .collect();
        let mut adj = vec![Vec::new(); nodes.len()];
        for (i, &(a, b)) in edges.iter().enumerate() {
            adj[a].push((b, i));
            adj[b].push((a, i));
        }
        let seed = nodes
            .iter()
            .enumerate()
            .min_by(|(_, a), (_, b)| distance(a.p, [1695.25, -883.64, 0.0]).total_cmp(&distance(b.p, [1695.25, -883.64, 0.0])))
            .unwrap()
            .0;
        // The first career stage reaching a node identifies its region. Floods
        // follow native barrier cuts, rather than guessing from XY rectangles.
        for stage in 1..=5 {
            let mut seen = vec![false; nodes.len()];
            let mut todo = vec![seed];
            seen[seed] = true;
            while let Some(a) = todo.pop() {
                if nodes[a].region == 0 {
                    nodes[a].region = stage;
                }
                for &(b, e) in &adj[a] {
                    if !seen[b] && masks[e] & (1 << stage) == 0 {
                        seen[b] = true;
                        todo.push(b);
                    }
                }
            }
        }
        if nodes.iter().any(|n| n.region == 0) {
            return Err(InvalidData("roads outside career regions"));
        }
        let edges: Vec<_> = edges
            .into_iter()
            .enumerate()
            .filter(|(i, (a, b))| nodes[*a].region == nodes[*b].region && masks[*i] & (1 << nodes[*a].region) == 0)
            .map(|(_, (a, b))| (a, b, distance(nodes[a].p, nodes[b].p)))
            .collect();
        let mut adj = vec![Vec::new(); nodes.len()];
        for &(a, b, d) in &edges {
            adj[a].push((b, d));
            adj[b].push((a, d));
        }
        Ok(Self { nodes, edges, adj, barriers: barriers.into_iter().map(|(p, _)| p).collect() })
    }
    /// Return a point on a reachable road, weighted by eligible segment length.
    /// `random` must be server-generated, uniform in [0,1). The caller keeps the
    /// selected point private until GO. Runs once per event, never per snapshot.
    pub fn destination(&self, start: [f64; 3], random: f64, excluded: &[[f64; 3]]) -> Option<[f64; 3]> {
        if !finite(&start) || !(0.0..1.0).contains(&random) {
            return None;
        }
        let seed = self
            .nodes
            .iter()
            .enumerate()
            .filter(|(_, n)| (n.p[2] - start[2]).abs() < 5.0)
            .min_by(|(_, a), (_, b)| distance(a.p, start).total_cmp(&distance(b.p, start)))?
            .0;
        if distance(self.nodes[seed].p, start) > 50.0 {
            return None;
        }
        let mut dist = vec![u64::MAX; self.nodes.len()];
        dist[seed] = 0;
        let mut heap = BinaryHeap::from([Reverse((0u64, seed))]);
        while let Some(Reverse((cost, a))) = heap.pop() {
            if cost != dist[a] {
                continue;
            }
            for &(b, d) in &self.adj[a] {
                let next = cost + (d * 1000.0).ceil() as u64;
                if next <= 3_000_000 && next < dist[b] {
                    dist[b] = next;
                    heap.push(Reverse((next, b)));
                }
            }
        }
        let candidates: Vec<_> = self
            .edges
            .iter()
            .filter_map(|&(a, b, len)| {
                if !(5.0..=100.0).contains(&len) || dist[a] < 1_000_000 || dist[b] < 1_000_000 || dist[a] > 2_950_000 || dist[b] > 2_950_000
                {
                    return None;
                }
                let p = std::array::from_fn(|i| (self.nodes[a].p[i] + self.nodes[b].p[i]) * 0.5);
                if distance(p, start) < 400.0 || excluded.iter().any(|&q| distance(p, q) < 80.0) {
                    return None;
                }
                // Keep the complete finish area away from every career boundary.
                if self.barriers.iter().any(|b| segment_distance(p, *b) < 40.0) {
                    return None;
                }
                Some((a, b, len))
            })
            .collect();
        let total: f64 = candidates.iter().map(|(_, _, len)| len).sum();
        if total <= 0.0 {
            return None;
        }
        let mut target = random * total;
        for (a, b, len) in candidates {
            if target < len {
                // The midpoint stays away from junction endpoints; random
                // selection is over road sections, not arbitrary world points.
                return Some(std::array::from_fn(|i| (self.nodes[a].p[i] + self.nodes[b].p[i]) * 0.5));
            }
            target -= len;
        }
        None
    }
}
fn segment_distance(p: [f64; 3], b: [f64; 4]) -> f64 {
    let (dx, dy) = (b[2] - b[0], b[3] - b[1]);
    let len = dx * dx + dy * dy;
    let t = if len > 0.0 { ((p[0] - b[0]) * dx + (p[1] - b[1]) * dy) / len } else { 0.0 }.clamp(0.0, 1.0);
    (p[0] - b[0] - t * dx).hypot(p[1] - b[1] - t * dy)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn malformed_data_is_rejected() {
        for bytes in [&[][..], &[0; 7], &[0xff; 32]] {
            assert!(Roads::parse(bytes, bytes).is_err());
        }
    }
    #[test]
    fn native_barrier_intersection() {
        assert!(crosses([0.0, 0.0, 0.0], [100.0, 0.0, 0.0], [50.0, -20.0, 50.0, 20.0]));
        assert!(!crosses([0.0, 0.0, 0.0], [100.0, 0.0, 0.0], [150.0, -20.0, 150.0, 20.0]));
    }
    #[test]
    #[ignore = "requires operator-owned game data: NFSU2_DATA"]
    fn original_game_regions_and_destinations() {
        let root = std::path::PathBuf::from(std::env::var_os("NFSU2_DATA").expect("NFSU2_DATA"));
        let root = root.join("TRACKS/ROUTESL4RA");
        let roads =
            Roads::parse(&std::fs::read(root.join("RoutesFreeRoam.bin")).unwrap(), &std::fs::read(root.join("PathsFreeRoam.bin")).unwrap())
                .unwrap();
        let counts: Vec<_> = (1..=5).map(|r| roads.nodes.iter().filter(|n| n.region == r).count()).collect();
        assert_eq!(counts, [4932, 1994, 1218, 1517, 942]);
        for region in 1..=5 {
            let mut found = false;
            for n in roads.nodes.iter().filter(|n| n.region == region).step_by(20) {
                if let Some(p) = roads.destination(n.p, 0.37, &[]) {
                    let nearest = roads.nodes.iter().min_by(|a, b| distance(a.p, p).total_cmp(&distance(b.p, p))).unwrap();
                    assert_eq!(nearest.region, region);
                    found = true;
                    break;
                }
            }
            assert!(found, "region {region} has no destination");
        }
    }
}
