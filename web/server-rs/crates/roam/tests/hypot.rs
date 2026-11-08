//! `hypot` against node's own Math.hypot, bit for bit (skipped with a message without node): differences small and
//! large, a zero or two among them, and the odd ones (subnormals, the largest double, infinities, NaN).  The links
//! are the nearest pairs, and where two pairs are as near the players' order decides: a distance a bit off would
//! link other players than the Node server's hub did.

use std::io::Write;
use std::process::{Command, Stdio};

use nfsu2_roam::hypot;

/// splitmix64: the same cases on every run
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    fn unit(&mut self) -> f64 {
        (self.next() >> 11) as f64 / (1u64 << 53) as f64
    }
    fn below(&mut self, n: usize) -> usize {
        ((self.unit() * n as f64) as usize).min(n.saturating_sub(1))
    }
}

/// the cases: differences of positions as the game has them (float32), of nearby ones, tiny ones, whole numbers and
/// zeros, and every pair of the odd values
fn hypots() -> Vec<[f64; 3]> {
    let mut r = Rng(99);
    let mut v: Vec<[f64; 3]> = (0..20_000)
        .map(|_| {
            let mut s = || match r.below(5) {
                0 => r.unit() * 40000.0 - 20000.0,
                1 => r.unit() * 2.0 - 1.0,
                2 => (r.unit() - 0.5) * 1e-6,
                3 => (r.unit() * 40000.0 - 20000.0) as f32 as f64 - (r.unit() * 40000.0 - 20000.0) as f32 as f64,
                _ => {
                    if r.below(5) == 0 {
                        0.0
                    } else {
                        r.below(1000) as f64 - 500.0
                    }
                }
            };
            [s(), s(), s()]
        })
        .collect();
    let odd = [0.0, -0.0, 1e-310, 5e-324, 1e300, 1.7976931348623157e308, f64::INFINITY, f64::NEG_INFINITY, f64::NAN, 3.0, 4.0, 12.0];
    for &x in &odd {
        for &y in &odd {
            v.push([x, y, 4.0]);
            v.push([x, 3.0, y]);
        }
    }
    v
}

#[test]
fn hypot_is_math_hypot() {
    if Command::new("node").arg("--version").output().is_err() {
        println!("skipped: no node");
        return;
    }
    let hyp = hypots();
    // the doubles as they are (little-endian), three a case in, one out
    let input: Vec<u8> = hyp.iter().flatten().flat_map(|x| x.to_le_bytes()).collect();
    let script = "const c = []; process.stdin.on('data', d => c.push(d)); process.stdin.on('end', () => {\n\
        const b = Buffer.concat(c), n = b.length / 24, out = Buffer.alloc(8 * n);\n\
        for (let i = 0; i < n; i++) out.writeDoubleLE(Math.hypot(b.readDoubleLE(24 * i), b.readDoubleLE(24 * i + 8), b.readDoubleLE(24 * i + 16)), 8 * i);\n\
        process.stdout.write(out); });";
    let mut child = Command::new("node")
        .args(["-e", script])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .expect("node");
    let mut stdin = child.stdin.take().expect("stdin");
    let writer = std::thread::spawn(move || stdin.write_all(&input));
    let out = child.wait_with_output().expect("node's answer");
    writer.join().expect("writer").expect("node's input");
    assert!(out.status.success(), "node failed");
    assert_eq!(out.stdout.len(), 8 * hyp.len(), "an answer for each case");
    let bad: Vec<String> = hyp
        .iter()
        .zip(out.stdout.as_chunks::<8>().0)
        .filter_map(|(h, b)| {
            let (a, b) = (hypot(h[0], h[1], h[2]), f64::from_le_bytes(*b));
            (a.to_bits() != b.to_bits()).then(|| format!("hypot({}, {}, {}): rust {a:e}, node {b:e}", h[0], h[1], h[2]))
        })
        .collect();
    println!("hypot: {} cases, {} differ", hyp.len(), bad.len());
    assert!(bad.is_empty(), "{} of {} differ, e.g.\n{}", bad.len(), hyp.len(), bad[..bad.len().min(10)].join("\n"));
}
