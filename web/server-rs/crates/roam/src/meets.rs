//! Cached car meets. One candidate per occupied 50 m × 50 m × 10 m cell bounds dense-crowd work.
//! Each player appears in one cell, queried from only neighbouring candidate cells;
//! a dense cell does not generate one candidate per player. Sorting is by density,
//! then cell coordinates for deterministic ties. Each player counts for one meet.
//! Candidate centres are cell centroids, so boundaries can differ from the former
//! client's per-player seeds. The 20-car threshold and 50 m radius are unchanged.
use std::collections::HashMap;
pub const MAX: usize = 8;
pub const SPECTATORS: usize = 24;
pub const EVERY_MS: f64 = 10_000.0;
const R: f64 = 50.0;
const MIN: usize = 20;
const HEIGHT: f64 = 10.0;
#[derive(Clone, Debug, PartialEq)]
pub struct Spectator {
    /// Centimetres in game world coordinates. Clients may project onto the
    /// road at this exact X/Y, but must never choose another position.
    pub p: [i32; 3],
    pub heading: u16,
    pub variant: u8,
}
#[derive(Clone, Debug, PartialEq)]
pub struct Meet {
    pub id: u32,
    pub spectators: Vec<Spectator>,
    pub p: [f64; 3],
    pub cars: usize,
}
fn cell(p: [f64; 3]) -> (i32, i32, i32) {
    ((p[0] / R).floor() as i32, (p[1] / R).floor() as i32, (p[2] / HEIGHT).floor() as i32)
}
fn near(a: [f64; 3], b: [f64; 3]) -> bool {
    (a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) <= R * R && (a[2] - b[2]).abs() < HEIGHT
}
fn center(ids: &[usize], points: &[[f64; 3]]) -> [f64; 3] {
    let mut p = [0.0; 3];
    for &i in ids {
        for (v, x) in p.iter_mut().zip(points[i]) {
            *v += x;
        }
    }
    for v in &mut p {
        *v /= ids.len() as f64;
    }
    p
}
pub fn detect(points: &[[f64; 3]]) -> Vec<Meet> {
    if points.len() < MIN {
        return Vec::new();
    }
    let mut grid: HashMap<(i32, i32, i32), Vec<usize>> = HashMap::new();
    for (i, &p) in points.iter().enumerate() {
        if p.iter().all(|x| x.is_finite() && x.abs() <= 32767.0) {
            grid.entry(cell(p)).or_default().push(i);
        }
    }
    let members = |p: [f64; 3], used: &[bool]| {
        let (x, y, z) = cell(p);
        let mut ids = Vec::new();
        for dx in -1..=1 {
            for dy in -1..=1 {
                for dz in -1..=1 {
                    if let Some(v) = grid.get(&(x + dx, y + dy, z + dz)) {
                        ids.extend(v.iter().copied().filter(|&i| !used[i] && near(points[i], p)));
                    }
                }
            }
        }
        ids
    };
    let mut used = vec![false; points.len()];
    let mut candidates: Vec<_> = grid
        .iter()
        .map(|(&key, ids)| {
            let p = center(ids, points);
            (members(p, &used).len(), key, p)
        })
        .filter(|c| c.0 >= MIN)
        .collect();
    candidates.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
    let mut out = Vec::new();
    for (_, _, p) in candidates {
        let ids = members(p, &used);
        if ids.len() < MIN {
            continue;
        }
        let p = center(&ids, points);
        let ids = members(p, &used);
        if ids.len() < MIN {
            continue;
        }
        for &i in &ids {
            used[i] = true;
        }
        out.push(Meet { id: 0, spectators: Vec::new(), p, cars: ids.len() });
        if out.len() == MAX {
            break;
        }
    }
    out
}

/// Preserve a crowd's identity and layout while people join and leave. A
/// detector's centroid and ordering can change; neither moves existing NPCs.
pub fn update(points: &[[f64; 3]], previous: &[Meet], next_id: &mut u32) -> Vec<Meet> {
    let mut list = detect(points);
    let mut used = vec![false; previous.len()];
    for m in &mut list {
        let old = previous.iter().enumerate().filter(|(i, p)| !used[*i] && near(p.p, m.p) && (p.p[2] - m.p[2]).abs() < 10.0).min_by(
            |(_, a), (_, b)| {
                let d = |p: &Meet| (p.p[0] - m.p[0]).powi(2) + (p.p[1] - m.p[1]).powi(2);
                d(a).total_cmp(&d(b))
            },
        );
        if let Some((i, old)) = old {
            used[i] = true;
            m.id = old.id;
            m.spectators = old.spectators.clone();
            continue;
        }
        *next_id = next_id.wrapping_add(1).max(1);
        m.id = *next_id;
        // Compact, uneven clusters like the race crowds: no rows or grid.
        // Resolve all variation once on the server and keep it for the meet.
        for group in 0..4 {
            // Six distinct stock crowd models per group. Shuffle on the
            // server once, then retain the resolved lineup with the meet.
            let mut variants = [0, 1, 2, 3, 4, 5];
            let mut seed = m.id.wrapping_mul(0x9e3779b9) ^ (group as u32 + 1).wrapping_mul(0x85ebca6b);
            for i in (1..6).rev() {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                variants.swap(i, seed as usize % (i + 1));
            }
            let mut random = || {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                seed as f64 / u32::MAX as f64 * 2.0 - 1.0
            };
            let angle = group as f64 * std::f64::consts::FRAC_PI_2;
            let (sin, cos) = angle.sin_cos();
            let (turn_sin, turn_cos) = (random() * 0.25).sin_cos();
            let mirror = if random() < 0.0 { -1.0 } else { 1.0 };
            // Uneven depth and spacing keep neighbours close without lining
            // them up. Rotation, mirroring and small offsets vary each cluster.
            let offsets: Vec<_> = [(0.0, -2.0), (0.65, -0.75), (-0.3, 0.55), (0.35, 1.9), (-1.0, -1.25), (1.1, 0.8)]
                .into_iter()
                .map(|(r, t)| {
                    let t = t * mirror;
                    (r * turn_cos - t * turn_sin + random() * 0.08, r * turn_sin + t * turn_cos + random() * 0.08, random() * 0.3)
                })
                .collect();
            for radius in [10.0, 18.0, 26.0, 34.0, 42.0, 50.0, 58.0] {
                let positions: Vec<_> = offsets
                    .iter()
                    .map(|&(depth, tangent, _)| {
                        let radial = radius + depth;
                        [m.p[0] + radial * cos - tangent * sin, m.p[1] + radial * sin + tangent * cos]
                    })
                    .collect();
                // Move a whole group together so avoiding parked cars does
                // not turn the crowd back into scattered individuals.
                if positions
                    .iter()
                    .any(|xy| points.iter().any(|p| (p[0] - xy[0]).powi(2) + (p[1] - xy[1]).powi(2) < 16.0 && (p[2] - m.p[2]).abs() < 4.0))
                {
                    continue;
                }
                for (i, [x, y]) in positions.into_iter().enumerate() {
                    let z = points
                        .iter()
                        .filter(|p| (p[2] - m.p[2]).abs() < 10.0)
                        .min_by(|a, b| ((a[0] - x).powi(2) + (a[1] - y).powi(2)).total_cmp(&((b[0] - x).powi(2) + (b[1] - y).powi(2))))
                        .map_or(m.p[2], |p| p[2]);
                    m.spectators.push(Spectator {
                        p: [x, y, z].map(|v| (v * 100.0).round().clamp(-3276700.0, 3276700.0) as i32),
                        heading: ((((m.p[1] - y).atan2(m.p[0] - x) + offsets[i].2).rem_euclid(std::f64::consts::TAU)
                            / std::f64::consts::TAU
                            * 65536.0)
                            .round() as u32
                            & 65535) as u16,
                        variant: variants[i],
                    });
                }
                break;
            }
        }
    }
    list
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn thresholds_boundaries_height_and_disjoint_groups() {
        assert!(detect(&vec![[0.0, 0.0, 12.0]; 19]).is_empty());
        let mut p = vec![[49.0, 49.0, 12.0]; 10];
        p.extend(vec![[51.0, 51.0, 16.0]; 10]);
        p.extend(vec![[-200.0, -200.0, 30.0]; 20]);
        let m = detect(&p);
        assert_eq!(m.len(), 2);
        assert_eq!(m.iter().map(|m| m.cars).sum::<usize>(), 40);
        assert!(m.iter().any(|m| m.p == [50.0, 50.0, 14.0]));
        assert!(detect(&[[f64::NAN, 0.0, 0.0]; 20]).is_empty());
    }
    #[test]
    fn stacked_roads_do_not_form_a_meet_together() {
        let mut p = vec![[100.0, 100.0, 12.0]; 10];
        p.extend(vec![[100.0, 100.0, 32.0]; 10]);
        assert!(detect(&p).is_empty());
        p.extend(vec![[100.0, 100.0, 12.0]; 10]);
        p.extend(vec![[100.0, 100.0, 32.0]; 10]);
        let m = detect(&p);
        assert_eq!(m.len(), 2);
        assert!(m.iter().all(|m| m.cars == 20));
        assert!(m.iter().any(|m| m.p[2] == 12.0));
        assert!(m.iter().any(|m| m.p[2] == 32.0));
    }
    #[test]
    fn caps_meets_and_never_counts_a_car_twice() {
        let p: Vec<_> = (0..20).flat_map(|g| vec![[g as f64 * 200.0, 0.0, 0.0]; 30]).collect();
        assert_eq!(detect(&p).len(), MAX);
        let p: Vec<_> = (0..300).map(|i| [(i % 100) as f64, (i / 100) as f64, 0.0]).collect();
        assert!(detect(&p).iter().map(|m| m.cars).sum::<usize>() <= p.len());
    }
    #[test]
    fn layouts_are_bounded_clear_of_parked_cars_and_stable() {
        let mut p = vec![[100.0, 100.0, 12.0]; 20];
        let mut id = 0;
        let first = update(&p, &[], &mut id);
        assert_eq!(first[0].spectators.len(), SPECTATORS);
        for pair in first[0].spectators.chunks(6) {
            let mut variants: Vec<_> = pair.iter().map(|p| p.variant).collect();
            variants.sort();
            assert_eq!(variants, vec![0, 1, 2, 3, 4, 5]);
            assert!(pair.windows(2).any(|p| p[0].heading != p[1].heading), "varied facing angles");
            for npc in pair {
                let nearest = pair
                    .iter()
                    .filter(|other| *other != npc)
                    .map(|other| ((npc.p[0] - other.p[0]) as f64).hypot((npc.p[1] - other.p[1]) as f64))
                    .fold(f64::INFINITY, f64::min);
                assert!((90.0..=190.0).contains(&nearest), "dense group without overlapping characters");
            }
        }
        for npc in &first[0].spectators {
            assert!(npc.variant < 6);
            assert!(p.iter().all(|p| (p[0] - npc.p[0] as f64 / 100.0).powi(2) + (p[1] - npc.p[1] as f64 / 100.0).powi(2) >= 16.0));
        }
        p.push([110.0, 110.0, 12.0]);
        let second = update(&p, &first, &mut id);
        assert_eq!(first[0].id, second[0].id);
        assert_eq!(first[0].spectators, second[0].spectators);
        let gone = update(&[], &second, &mut id);
        assert!(gone.is_empty());
        let returned = update(&p, &gone, &mut id);
        assert_ne!(first[0].id, returned[0].id);
    }
    #[test]
    #[ignore = "performance measurement; run with --ignored --nocapture"]
    fn benchmark_meets() {
        for n in [1000, 4096, 16000] {
            for layout in 0..3 {
                let p: Vec<_> = (0..n)
                    .map(|i| match layout {
                        0 => [100.0 + (i % 10) as f64, 100.0 + (i % 7) as f64, 10.0],
                        1 => [(i % 128) as f64 * 60.0, (i / 128) as f64 * 60.0, 10.0],
                        _ => [(i % 400) as f64 * 2.0, (i / 400) as f64 * 2.0, 10.0],
                    })
                    .collect();
                let mut id = 0;
                let start = std::time::Instant::now();
                let cached = update(&p, &[], &mut id);
                let initial = start.elapsed().as_secs_f64() * 1000.0;
                let start = std::time::Instant::now();
                for _ in 0..20 {
                    std::hint::black_box(update(std::hint::black_box(&p), &cached, &mut id));
                }
                eprintln!(
                    "meet detection + layout n={n} layout={layout}: {:.3} ms/update, {:.3} ms first layout",
                    start.elapsed().as_secs_f64() * 1000.0 / 20.0,
                    initial
                );
            }
        }
    }
}
