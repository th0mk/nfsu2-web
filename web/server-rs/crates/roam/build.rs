use std::{env, fs, path::PathBuf};
fn main() {
    println!("cargo:rerun-if-changed=src/speedtraps.def");
    let mut out = String::from("pub const TRAPS: &[Trap] = &[\n");
    let mut ids = std::collections::HashSet::new();
    for line in include_str!("src/speedtraps.def").lines().map(str::trim).filter(|s| !s.is_empty() && !s.starts_with("//")) {
        let values: Vec<_> = line.strip_prefix("TRAP(").unwrap().strip_suffix(')').unwrap().split(',').map(str::trim).collect();
        assert_eq!(values.len(), 7);
        let id = values[0];
        assert!(!id.is_empty() && id.len() <= 32 && id.bytes().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'_'));
        assert!(ids.insert(id));
        for v in &values[1..] {
            assert!(v.parse::<f64>().unwrap().is_finite());
        }
        out.push_str(&format!(
            "Trap {{ id: std::borrow::Cow::Borrowed({id:?}), name: \"Speed trap\", ends: [[{},{},{}],[{},{},{}]] }},\n",
            values[1], values[2], values[3], values[4], values[5], values[6]
        ));
    }
    assert!(ids.len() <= 64);
    out.push_str("];\n");
    fs::write(PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("speedtraps.rs"), out).unwrap();
}
