//! Where the players are and how long their paths are, for the server's 5-minute counts (its `[players]` lines): each
//! player's round trip to the server by its country (Cloudflare's CF-IPCountry: `AttachOpts::country`) and by region,
//! and of the pairs that play together, free roam's links (the hub's: `Host::link_pairs`) and LAN play's (a race, its
//! lobby: UDP from one player to another), how many are of one country and of one region, and how long the way from one
//! player to the other through the server is (the two players' round trips together).  What it would bring to connect
//! players near each other some other way (a direct link, a server in their region) is in those numbers.
//!
//! A round trip is the player's lowest of the last 30-60 s (lag.rs `Probe::base`): its path without the queues on it.
//! Nothing here costs a datagram more than `race_with` (a load or two on the sender's own peer, a store when the pair
//! is new); the rest is made when the counts are, every 5 minutes.  Counts only: never a player's own.

use std::collections::HashMap;
use std::fmt::Write as _;
use std::sync::atomic::{AtomicU32, Ordering::Relaxed};

/// a country as `Peer::country` keeps it: its two letters, big-endian (0: not known)
pub fn code(c: [u8; 2]) -> u16 {
    u16::from_be_bytes(c)
}

fn letters(c: u16) -> String {
    let [a, b] = c.to_be_bytes();
    format!("{}{}", a as char, b as char)
}

/// The regions the counts group countries in: the UN's geoscheme's, a few put together (Central America with the
/// Caribbean, Central with South Asia), Western Asia as the Middle East with Turkey, the Caucasus and Iran, and all of
/// Russia in Eastern Europe (most of its players are in its west).
pub const REGIONS: [&str; 16] = [
    "unknown",
    "Northern Europe",
    "Western Europe",
    "Southern Europe",
    "Eastern Europe",
    "Western Asia",
    "Central and South Asia",
    "East Asia",
    "Southeast Asia",
    "Oceania",
    "North America",
    "Central America and the Caribbean",
    "South America",
    "North Africa",
    "Sub-Saharan Africa",
    "elsewhere",
];

/// a country's place in `REGIONS` (0: not known; the last: a code of no region here, as Antarctica's)
pub fn region(c: u16) -> usize {
    if c == 0 {
        return 0;
    }
    match &c.to_be_bytes() {
        b"GB" | b"IE" | b"IS" | b"NO" | b"SE" | b"FI" | b"DK" | b"EE" | b"LV" | b"LT" | b"FO" | b"AX" | b"IM" | b"JE" | b"GG" | b"SJ" => 1,
        b"FR" | b"DE" | b"NL" | b"BE" | b"LU" | b"CH" | b"AT" | b"LI" | b"MC" => 2,
        b"ES" | b"PT" | b"IT" | b"GR" | b"MT" | b"AD" | b"SM" | b"VA" | b"GI" | b"SI" | b"HR" | b"BA" | b"RS" | b"ME" | b"MK" | b"AL"
        | b"XK" => 3,
        b"PL" | b"CZ" | b"SK" | b"HU" | b"RO" | b"BG" | b"MD" | b"UA" | b"BY" | b"RU" => 4,
        b"TR" | b"CY" | b"GE" | b"AM" | b"AZ" | b"IL" | b"PS" | b"JO" | b"LB" | b"SY" | b"IQ" | b"IR" | b"SA" | b"AE" | b"QA" | b"KW"
        | b"BH" | b"OM" | b"YE" => 5,
        b"KZ" | b"UZ" | b"TM" | b"KG" | b"TJ" | b"AF" | b"PK" | b"IN" | b"BD" | b"LK" | b"NP" | b"BT" | b"MV" => 6,
        b"CN" | b"JP" | b"KR" | b"KP" | b"TW" | b"HK" | b"MO" | b"MN" => 7,
        b"ID" | b"MY" | b"SG" | b"TH" | b"VN" | b"PH" | b"MM" | b"KH" | b"LA" | b"BN" | b"TL" => 8,
        b"AU" | b"NZ" | b"PG" | b"FJ" | b"SB" | b"VU" | b"NC" | b"PF" | b"WS" | b"TO" | b"KI" | b"FM" | b"MH" | b"PW" | b"NR" | b"TV"
        | b"GU" | b"MP" | b"AS" | b"CK" | b"NU" | b"NF" | b"WF" | b"TK" | b"PN" => 9,
        b"US" | b"CA" | b"GL" | b"BM" | b"PM" | b"UM" => 10,
        b"MX" | b"GT" | b"BZ" | b"SV" | b"HN" | b"NI" | b"CR" | b"PA" | b"CU" | b"DO" | b"HT" | b"JM" | b"PR" | b"TT" | b"BS" | b"BB"
        | b"LC" | b"VC" | b"GD" | b"AG" | b"DM" | b"KN" | b"AW" | b"CW" | b"SX" | b"BQ" | b"KY" | b"TC" | b"VG" | b"VI" | b"AI" | b"MS"
        | b"MF" | b"BL" | b"GP" | b"MQ" => 11,
        b"BR" | b"AR" | b"CL" | b"CO" | b"PE" | b"VE" | b"EC" | b"BO" | b"PY" | b"UY" | b"GY" | b"SR" | b"GF" | b"FK" | b"GS" => 12,
        b"EG" | b"LY" | b"TN" | b"DZ" | b"MA" | b"SD" | b"EH" => 13,
        b"NG" | b"GH" | b"CI" | b"SN" | b"ML" | b"BF" | b"NE" | b"TD" | b"CM" | b"CF" | b"GA" | b"CG" | b"CD" | b"AO" | b"ZM" | b"ZW"
        | b"MZ" | b"MW" | b"TZ" | b"KE" | b"UG" | b"RW" | b"BI" | b"ET" | b"ER" | b"DJ" | b"SO" | b"SS" | b"ZA" | b"NA" | b"BW" | b"LS"
        | b"SZ" | b"MG" | b"MU" | b"SC" | b"KM" | b"RE" | b"YT" | b"CV" | b"GM" | b"GN" | b"GW" | b"SL" | b"LR" | b"TG" | b"BJ" | b"MR"
        | b"ST" | b"GQ" | b"SH" => 14,
        _ => REGIONS.len() - 1,
    }
}

/// the players a peer's LAN play sent UDP to since the last count, at most this many (a race has six players)
pub(crate) const LAN_TO: usize = 8;

/// LAN play's UDP from a player to `dst`: noted in its `lan_to` (only its own reader calls this).  Two places a
/// destination may be at; a third that hashes there takes one of them (a count is not worse for it).
pub(crate) fn race_with(to: &[AtomicU32; LAN_TO], dst: u32) {
    let h = (dst.wrapping_mul(0x9E37_79B1) >> 29) as usize;
    let (a, b) = (&to[h], &to[h ^ 1]);
    let (x, y) = (a.load(Relaxed), b.load(Relaxed));
    if x == dst || y == dst {
        return;
    }
    if x == 0 { a.store(dst, Relaxed) } else { b.store(dst, Relaxed) }
}

/// what `race_with` noted since the last time, into `out`, and none kept
pub(crate) fn take(to: &[AtomicU32; LAN_TO], out: &mut Vec<u32>) {
    out.extend(to.iter().map(|s| s.swap(0, Relaxed)).filter(|&d| d != 0));
}

/// One player as the counts see it: its address, country (`code`; 0 not known) and round trip (lag.rs `Probe::base`:
/// none before its first pong).
#[derive(Clone, Copy, Debug)]
pub struct Seen {
    pub vip: u32,
    pub country: u16,
    pub base: Option<f64>,
}

/// the median and the 90th percentile (nearest rank) of `v`, sorted here; None for none
fn p50_90(v: &mut [f64]) -> Option<(f64, f64)> {
    if v.is_empty() {
        return None;
    }
    v.sort_unstable_by(f64::total_cmp);
    let at = |p: f64| v[((p * v.len() as f64).ceil() as usize).clamp(1, v.len()) - 1];
    Some((at(0.5), at(0.9)))
}

/// "n p50/p90" of round trips (ms, whole)
fn spread(v: &mut [f64]) -> String {
    match p50_90(v) {
        Some((a, b)) => format!("{} {:.0}/{:.0}", v.len(), a, b),
        None => "0".into(),
    }
}

/// the countries most players are in, the most first, then one entry for the rest
const TOP_COUNTRIES: usize = 12;

/// The counts' lines (without "[players] "): the round trips by country and by region, and the pairs' of free roam
/// (`roam`, the hub's links now) and of LAN play (`lan`, since the last count), each pair once.  None while no player
/// has a round trip yet.
pub fn lines(players: &[Seen], roam: &[(u32, u32)], lan: &[(u32, u32)]) -> Vec<String> {
    let mut out = Vec::new();
    let timed: Vec<&Seen> = players.iter().filter(|p| p.base.is_some()).collect();
    if timed.is_empty() {
        return out;
    }
    let untimed = players.len() - timed.len();
    let mut all: Vec<f64> = timed.iter().filter_map(|p| p.base).collect();
    // by country, the unknown ones last
    let mut by: HashMap<u16, Vec<f64>> = HashMap::new();
    for p in &timed {
        by.entry(p.country).or_default().extend(p.base);
    }
    let mut unknown = by.remove(&0).unwrap_or_default();
    let mut countries: Vec<(u16, Vec<f64>)> = by.into_iter().collect();
    countries.sort_unstable_by(|a, b| b.1.len().cmp(&a.1.len()).then(a.0.cmp(&b.0)));
    let mut l = format!(
        "round trips to the server by country (each player's lowest of the last 30-60 s, players and ms p50/p90): all {}",
        spread(&mut all)
    );
    for (c, v) in countries.iter_mut().take(TOP_COUNTRIES) {
        let _ = write!(l, "; {} {}", letters(*c), spread(v));
    }
    if countries.len() > TOP_COUNTRIES {
        let mut rest: Vec<f64> = countries[TOP_COUNTRIES..].iter().flat_map(|(_, v)| v.iter().copied()).collect();
        let _ = write!(l, "; {} more countries {}", countries.len() - TOP_COUNTRIES, spread(&mut rest));
    }
    if !unknown.is_empty() {
        let _ = write!(l, "; unknown {}", spread(&mut unknown));
    }
    if untimed > 0 {
        let _ = write!(l, "; {untimed} not timed yet");
    }
    out.push(l);
    // by region, the most first
    let mut regions: Vec<Vec<f64>> = vec![Vec::new(); REGIONS.len()];
    for p in &timed {
        regions[region(p.country)].extend(p.base);
    }
    let mut order: Vec<usize> = (1..REGIONS.len()).filter(|&r| !regions[r].is_empty()).collect();
    order.sort_by_key(|&r| std::cmp::Reverse(regions[r].len()));
    if !regions[0].is_empty() {
        order.push(0);
    }
    let mut l = String::from("round trips to the server by region:");
    for (i, r) in order.into_iter().enumerate() {
        let _ = write!(l, "{} {} {}", if i == 0 { "" } else { ";" }, REGIONS[r], spread(&mut regions[r]));
    }
    out.push(l);
    // the pairs
    let at: HashMap<u32, &Seen> = players.iter().map(|p| (p.vip, p)).collect();
    if let Some(l) = pairs(&at, roam) {
        out.push(format!("free-roam links now: {l}"));
    }
    if let Some(l) = pairs(&at, lan) {
        out.push(format!("LAN play's pairs since the last count (races, their lobbies): {l}"));
    }
    out
}

/// One kind of pairs: how many, the shares of one country and one region (of those whose two countries are known),
/// and the way from one player to the other through the server (both round trips together): all, within one region,
/// across regions.  None for no pair of two players on the switch.
fn pairs(at: &HashMap<u32, &Seen>, list: &[(u32, u32)]) -> Option<String> {
    let (mut n, mut known, mut country, mut region_) = (0usize, 0usize, 0usize, 0usize);
    let (mut all, mut same, mut across) = (Vec::new(), Vec::new(), Vec::new());
    for (a, b) in list {
        let (Some(a), Some(b)) = (at.get(a), at.get(b)) else { continue };
        n += 1;
        let both = a.country != 0 && b.country != 0;
        let one = both && region(a.country) == region(b.country);
        if both {
            known += 1;
            country += (a.country == b.country) as usize;
            region_ += one as usize;
        }
        if let (Some(x), Some(y)) = (a.base, b.base) {
            all.push(x + y);
            if both {
                if one { same.push(x + y) } else { across.push(x + y) }
            }
        }
    }
    if n == 0 {
        return None;
    }
    let pct = |k: usize| if known == 0 { 0.0 } else { 100.0 * k as f64 / known as f64 };
    let mut l = format!("{n} {} of players", if n == 1 { "pair" } else { "pairs" });
    if known > 0 {
        let _ = write!(
            l,
            ", {:.0} % in one country and {:.0} % in one region (of the {known} whose countries are known)",
            pct(country),
            pct(region_)
        );
    }
    let _ = write!(
        l,
        "; from one player to the other through the server (their round trips together, pairs and ms p50/p90): all {}, \
         within one region {}, across regions {}",
        spread(&mut all),
        spread(&mut same),
        spread(&mut across)
    );
    Some(l)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seen(vip: u32, c: &[u8; 2], base: Option<f64>) -> Seen {
        Seen { vip, country: if c == b"--" { 0 } else { code(*c) }, base }
    }

    #[test]
    fn percentiles_by_nearest_rank() {
        assert_eq!(p50_90(&mut []), None);
        assert_eq!(p50_90(&mut [7.0]), Some((7.0, 7.0)));
        assert_eq!(p50_90(&mut [4.0, 1.0, 3.0, 2.0]), Some((2.0, 4.0)));
        let mut v: Vec<f64> = (1..=10).rev().map(f64::from).collect();
        assert_eq!(p50_90(&mut v), Some((5.0, 9.0)));
        let mut v: Vec<f64> = (1..=100).map(f64::from).collect();
        assert_eq!(p50_90(&mut v), Some((50.0, 90.0)));
    }

    #[test]
    fn countries_and_regions() {
        assert_eq!(REGIONS[region(code(*b"DE"))], "Western Europe");
        assert_eq!(REGIONS[region(code(*b"RU"))], "Eastern Europe");
        assert_eq!(REGIONS[region(code(*b"TR"))], "Western Asia");
        assert_eq!(REGIONS[region(code(*b"BR"))], "South America");
        assert_eq!(REGIONS[region(code(*b"MX"))], "Central America and the Caribbean");
        assert_eq!(REGIONS[region(code(*b"ZA"))], "Sub-Saharan Africa");
        assert_eq!(REGIONS[region(code(*b"AQ"))], "elsewhere");
        assert_eq!(region(0), 0);
        assert_eq!(letters(code(*b"NL")), "NL");
    }

    // a destination noted once, at most LAN_TO of them, all taken at once
    #[test]
    fn lan_pairs_noted_and_taken() {
        let to: [AtomicU32; LAN_TO] = Default::default();
        for d in [5, 6, 5, 7, 6, 5] {
            race_with(&to, d);
        }
        let mut got = Vec::new();
        take(&to, &mut got);
        got.sort_unstable();
        assert_eq!(got, [5, 6, 7]);
        take(&to, &mut got);
        assert_eq!(got.len(), 3, "taken: none left");
        // a race's five others, each noted (they may share a place: then each is there or its neighbour)
        let to: [AtomicU32; LAN_TO] = Default::default();
        let others = [0x0A4D_0003u32, 0x0A4D_0104, 0x0A4D_0205, 0x0A4D_0306, 0x0A4D_0407];
        for _ in 0..3 {
            others.iter().for_each(|&d| race_with(&to, d));
        }
        let mut got = Vec::new();
        take(&to, &mut got);
        assert!(got.len() >= 3 && got.iter().all(|d| others.contains(d)), "{got:x?}");
    }

    #[test]
    fn the_lines() {
        let p = [
            seen(1, b"DE", Some(20.0)),
            seen(2, b"DE", Some(30.0)),
            seen(3, b"NL", Some(15.0)),
            seen(4, b"BR", Some(200.0)),
            seen(5, b"BR", Some(220.0)),
            seen(6, b"--", Some(90.0)),
            seen(7, b"RU", None),
        ];
        let l = lines(&p, &[(1, 2), (1, 3), (4, 5), (1, 4), (3, 6), (2, 99)], &[(4, 5)]);
        assert_eq!(l.len(), 4, "{l:#?}");
        assert_eq!(
            l[0],
            "round trips to the server by country (each player's lowest of the last 30-60 s, players and ms p50/p90): \
             all 6 30/220; BR 2 200/220; DE 2 20/30; NL 1 15/15; unknown 1 90/90; 1 not timed yet"
        );
        assert_eq!(l[1], "round trips to the server by region: Western Europe 3 20/30; South America 2 200/220; unknown 1 90/90");
        // (2, 99): a player gone; (3, 6): a country not known, in the ways only
        assert_eq!(
            l[2],
            "free-roam links now: 5 pairs of players, 50 % in one country and 75 % in one region (of the 4 whose countries \
             are known); from one player to the other through the server (their round trips together, pairs and ms \
             p50/p90): all 5 105/420, within one region 3 50/420, across regions 1 220/220"
        );
        assert!(l[3].starts_with("LAN play's pairs since the last count (races, their lobbies): 1 pair of players, 100 % in one country"));
        // nobody timed: nothing
        assert!(lines(&[seen(1, b"DE", None)], &[], &[]).is_empty());
        // the rest past the top countries in one entry
        let many: Vec<Seen> = (0..20u8).map(|i| seen(u32::from(i) + 1, &[b'A' + i, b'A'], Some(f64::from(i)))).collect();
        let l = lines(&many, &[], &[]);
        assert!(l[0].ends_with("; 8 more countries 8 15/19"), "{}", l[0]);
    }
}
