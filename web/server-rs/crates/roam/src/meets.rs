//! Cached car meets. One candidate per occupied 50 m × 50 m × 10 m cell bounds dense-crowd work.
//! Each player appears in one cell, queried from only neighbouring candidate cells;
//! a dense cell does not generate one candidate per player. Sorting is by density,
//! then cell coordinates for deterministic ties. Each player counts for one meet.
//! Candidate centres are cell centroids, so boundaries can differ from the former
//! client's per-player seeds. The 20-car threshold and 50 m radius are unchanged.
use std::collections::HashMap;
pub const MAX: usize = 8;
pub const EVERY_MS: f64 = 10_000.0;
const R: f64 = 50.0;
const MIN: usize = 20;
const HEIGHT: f64 = 10.0;
#[derive(Clone, Debug, PartialEq)]
pub struct Meet {
    pub id: u32,
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
        out.push(Meet { id: 0, p, cars: ids.len() });
        if out.len() == MAX {
            break;
        }
    }
    out
}

/// Preserve meet identity while its membership and centroid change.
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
            continue;
        }
        *next_id = next_id.wrapping_add(1).max(1);
        m.id = *next_id;

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
    fn meet_identity_survives_membership_changes() {
        let mut p = vec![[100.0, 100.0, 12.0]; 20];
        let mut id = 0;
        let first = update(&p, &[], &mut id);
        p.push([110.0, 110.0, 12.0]);
        let second = update(&p, &first, &mut id);
        assert_eq!(first[0].id, second[0].id);
        let gone = update(&[], &second, &mut id);
        assert!(gone.is_empty());
        assert_ne!(first[0].id, update(&p, &gone, &mut id)[0].id);
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
