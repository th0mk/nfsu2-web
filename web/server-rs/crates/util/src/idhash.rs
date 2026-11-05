//! A hasher for maps keyed by the server's own small numbers (virtual addresses, slots, rooms, the game's player ids):
//! a multiplication and a shift where std's maps run SipHash.  The free-roam hub looks its players' blocks up twice for
//! every pair it weighs (hub.rs `blocked`), and SipHash of those keys took a tenth of its thread on nfsu2.xyz
//! (2026-11-05, 11,500 players).  None of these keys is one a client can pick by the thousand (a page holds one
//! address, its game one id), so a weaker hash leaves no flood of colliding keys to fear.

use std::collections::{HashMap, HashSet};
use std::hash::{BuildHasherDefault, Hasher};

/// The hasher: each integer written mixed in by one multiplication (the golden ratio's, odd) and the product's high
/// half folded onto its low one, so that both the low bits (the table's slot) and the top ones (hashbrown's tag) vary.
#[derive(Default, Clone, Copy)]
pub struct IdHasher(u64);

impl Hasher for IdHasher {
    fn finish(&self) -> u64 {
        self.0
    }
    fn write(&mut self, b: &[u8]) {
        for &x in b {
            self.write_u64(x as u64);
        }
    }
    fn write_u8(&mut self, k: u8) {
        self.write_u64(k as u64);
    }
    fn write_u16(&mut self, k: u16) {
        self.write_u64(k as u64);
    }
    fn write_u32(&mut self, k: u32) {
        self.write_u64(k as u64);
    }
    fn write_usize(&mut self, k: usize) {
        self.write_u64(k as u64);
    }
    fn write_u64(&mut self, k: u64) {
        let x = (self.0 ^ k).wrapping_mul(0x9E37_79B9_7F4A_7C15);
        self.0 = x ^ x >> 32;
    }
}

pub type IdBuild = BuildHasherDefault<IdHasher>;
/// a map by such a number
pub type IdMap<K, V> = HashMap<K, V, IdBuild>;
/// a set of them
pub type IdSet<K> = HashSet<K, IdBuild>;

#[cfg(test)]
mod tests {
    use super::*;
    use std::hash::BuildHasher;

    #[test]
    fn neighbours_spread_over_slots_and_tags() {
        // a run of addresses (10.77.x.y one after the other) lands in as many slots of a 4096-slot table, and their top
        // 7 bits (hashbrown's tag) take most of their 128 values
        let b = IdBuild::default();
        let (mut slots, mut tags) = (std::collections::HashSet::new(), std::collections::HashSet::new());
        for v in 0x0A4D_0000u32..0x0A4D_1000 {
            let h = b.hash_one(v);
            slots.insert(h & 0xFFF);
            tags.insert(h >> 57);
        }
        assert!(slots.len() > 4096 * 6 / 10, "{} slots", slots.len());
        assert!(tags.len() > 120, "{} tags", tags.len());
    }

    #[test]
    fn a_map_by_it_works_as_any() {
        let mut m: IdMap<u32, u32> = IdMap::default();
        for k in 0..10_000u32 {
            m.insert(k.wrapping_mul(2_654_435_761), k);
        }
        assert_eq!(m.len(), 10_000);
        assert!((0..10_000u32).all(|k| m[&k.wrapping_mul(2_654_435_761)] == k));
        let s: IdSet<u32> = (0..100).collect();
        assert!(s.contains(&99) && !s.contains(&100));
    }
}
