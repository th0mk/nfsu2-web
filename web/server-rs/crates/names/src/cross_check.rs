//! names.mjs itself, under node, against this port: the same answer for every name of a corpus (names.test.mjs's
//! names, near misses, made-up names from a fixed seed, ASCII and Unicode, and every code point of Unicode's planes
//! in use in a few places of a name), the same lists and the same lookalikes.  Skipped (with a message) where there
//! is no node.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Command;

use super::tests::{ALLOWED, BLOCKED, UNICODE};
use super::*;

/// SplitMix64: the same names on every run
struct Rnd(u64);

impl Rnd {
    fn next(&mut self, n: usize) -> usize {
        self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        ((z ^ (z >> 31)) % n as u64) as usize
    }

    fn pick<'a, T>(&mut self, a: &'a [T]) -> &'a T {
        &a[self.next(a.len())]
    }
}

/// the characters of these ranges of code points
fn chars(ranges: &[(u32, u32)]) -> Vec<char> {
    ranges.iter().flat_map(|&(a, b)| a..=b).filter_map(char::from_u32).collect()
}

/// Marks: accents of every script, variation selectors, the keycap, and some that Unicode added after 16.0 (marks
/// with a combining class in unicode-normalization's tables, nothing yet for Node's ICU).
fn marks() -> Vec<char> {
    chars(&[
        (0x300, 0x36f),
        (0x483, 0x489),
        (0x591, 0x5bd),
        (0x64b, 0x65f),
        (0x900, 0x903),
        (0x93a, 0x94f),
        (0x1ab0, 0x1ace),
        (0x1dc0, 0x1dff),
        (0x20d0, 0x20f0),
        (0xfe00, 0xfe0f),
        (0xfe20, 0xfe2f),
        (0x1d165, 0x1d169),
        (0x1d16d, 0x1d172),
        (0xe0100, 0xe0110),
        (0x10ef5, 0x10efb),
        (0x1e6e3, 0x1e6ef),
    ])
}

/// Spaces, separators and signs that stand for no letter, the format characters (zero-width, bidi, tags) among them.
fn separators() -> Vec<char> {
    let mut v: Vec<char> = " _-.,;:/\\*#~^`'\"?=<>()[]{}%&2\t\n".chars().collect();
    v.extend(chars(&[
        (0xa0, 0xa0),
        (0xad, 0xad),
        (0xb7, 0xb7),
        (0x61c, 0x61c),
        (0x85, 0x85),
        (0x1680, 0x1680),
        (0x180e, 0x180e),
        (0x2000, 0x200f),
        (0x2022, 0x2022),
        (0x2028, 0x202f),
        (0x205f, 0x2064),
        (0x2066, 0x2069),
        (0x3000, 0x3000),
        (0xfeff, 0xfeff),
        (0xe0001, 0xe0001),
        (0xe0020, 0xe007f),
    ]));
    v
}

/// Emoji (with a skin tone, a joiner, a variation selector), other scripts, digits of other scripts, signs that
/// NFKD spells out (™ ½ ㎏), private use, noncharacters, holes in the styled alphabets, and characters Unicode
/// added after 16.0 that unicode-normalization would decompose (to Latin letters among others).
fn others() -> Vec<char> {
    chars(&[
        (0x1f600, 0x1f60f),
        (0x1f525, 0x1f525),
        (0x1f44d, 0x1f44d),
        (0x1f3fb, 0x1f3ff),
        (0x1f3ce, 0x1f3ce),
        (0x1f697, 0x1f697),
        (0x1f1e6, 0x1f1ff),
        (0x1f170, 0x1f189),
        (0x2764, 0x2764),
        (0x200d, 0x200d),
        (0xfe0f, 0xfe0f),
        (0x2122, 0x2122),
        (0xa9, 0xae),
        (0xbc, 0xbe),
        (0x2150, 0x215f),
        (0x3380, 0x33df),
        (0x4e00, 0x4e0f),
        (0xac00, 0xac0f),
        (0xd55c, 0xd55c),
        (0x3131, 0x3140),
        (0x5d0, 0x5ea),
        (0x627, 0x64a),
        (0x660, 0x669),
        (0x966, 0x96f),
        (0xe01, 0xe0f),
        (0xdf, 0xdf),
        (0x1e9e, 0x1e9e),
        (0xfffd, 0xfffd),
        (0xe000, 0xe000),
        (0xf8ff, 0xf8ff),
        (0xf0000, 0xf0000),
        (0xfdd0, 0xfdd0),
        (0xfffe, 0xffff),
        (0x378, 0x379),
        (0x1d455, 0x1d455),
        (0x1d49d, 0x1d49d),
        (0x2ffff, 0x2ffff),
        (0x1b123, 0x1b126),
        (0x1d6a6, 0x1d6a6),
        (0x1df95, 0x1df95),
        (0x1dfcd, 0x1dfff),
    ])
}

/// The code points this port reads as one ASCII character, by that character: styled letters and digits
/// (fullwidth, mathematical, circled, squared, super- and subscript), accented letters, ligature-free letterlike
/// signs, and the lookalikes of other scripts.  That names.mjs reads them so too is what the answers show.
fn alternatives() -> HashMap<char, Vec<char>> {
    let mut alts: HashMap<char, Vec<char>> = HashMap::new();
    let ranges = [
        (0xa0, 0x2ff),
        (0x370, 0x52f),
        (0x1d00, 0x1dbf),
        (0x1e00, 0x1eff),
        (0x2070, 0x20c0),
        (0x2100, 0x218f),
        (0x2460, 0x24ff),
        (0xff00, 0xff5e),
        (0x1d400, 0x1d7ff),
        (0x1f100, 0x1f1ff),
    ];
    for ch in chars(&ranges) {
        match read(&ch.to_string())[..] {
            [r] if r.is_ascii_graphic() => alts.entry(r).or_default().push(ch),
            _ => {}
        }
    }
    alts
}

/// the near misses of names.mjs's comments, and the same with other case, spacing and numbers
const NEAR: &[&str] = &[
    "Niger",
    "Nigeria",
    "NIGER",
    "nigerr",
    "Van Dyke",
    "VanDyke",
    "van dyke",
    "VAN DYKE",
    "Van_Dyke",
    "Van Dykes",
    "Vandyke",
    "Van  Dyke",
    "Van1Dyke",
    "Vann Dyke",
    "Mr Van Dyke",
    "raccoon",
    "Raccoon",
    "RACCOON",
    "rac coon",
    "racCoon",
    "Hakan1995",
    "Hakan 1995",
    "hakan19",
    "Hakan95",
    "Dani99GS",
    "Dani99gs",
    "dani99GS",
    "DANI99GS",
    "Dani99Gs",
    "Dani 99GS",
    "ni99a",
    "Ni99a",
    "nI99A",
    "ni9a",
    "ni999a",
    "ni99A",
    "NI99a",
    "F A G",
    "F A G S",
    "F.A.G",
    "f a g g o t",
    "F A G X",
    "XxCoonXx",
    "xXcOoNxX",
    "XXCOONXX",
    "xxCoonxx",
    "XxCoon",
    "CoonXx",
    "Big1Fag",
    "big1fag",
    "BIG1FAG",
    "Big12Fag",
    "Big123Fag",
    "Big1Fagz",
    "Big1Fags",
    "Mustafa GS",
    "MUSTAFAGS",
    "Pakize",
];

/// pieces of the made-up names: the lists' words, the near misses, the names this server sees most, digits
fn pieces() -> Vec<&'static str> {
    let mut p: Vec<&str> =
        ANYWHERE.iter().chain(WHOLE).chain(EDGE).chain(EDGE_OK_START).chain(EDGE_OK_END).chain(PID_STEMS).copied().collect();
    p.extend(NEAR);
    p.extend([
        "mustafa", "gs", "raccoon", "spice", "spicy", "niger", "pakize", "pakistan", "xx", "gamer", "a", "g", "dani", "hakan", "van",
        "tycoon", "trigger", "ibrahim", "99", "1995", "69", "42",
    ]);
    // the neo-Nazi code and other numbers, names with the new words' letters in them
    p.extend(["1488", "88", "spider", "shinigami", "zuniga", "adolf"]);
    // a lone n before digits, the "pid" words' surroundings, Nigeria
    p.extend(["n", "1663", "pid", "topi", "drift", "nigeria"]);
    p
}

/// the digits and signs that stand for a letter (names.mjs FROM and TO, the other way round)
fn swap(ch: char) -> &'static [char] {
    match ch.to_ascii_lowercase() {
        'o' => &['0'],
        'i' => &['1', '!', '|', 'l'],
        'e' => &['3'],
        'a' => &['4', '@'],
        's' => &['5', '$'],
        't' => &['7', '+'],
        'g' => &['6', '9', 'q'],
        'b' => &['8'],
        _ => &[],
    }
}

/// names of names.test.mjs's generator (web/test/names.test.mjs, its 20,000 for names.h): the pieces with digits and
/// signs for letters, capitals, doubled letters, separators, a number at the end; then plain ASCII noise
fn ascii_names(r: &mut Rnd, pieces: &[&str], names: &mut Vec<String>) {
    for _ in 0..20_000 {
        let mut n = String::new();
        for _ in 0..1 + r.next(3) {
            for ch in r.pick(pieces).chars() {
                match r.next(10) {
                    0 if !swap(ch).is_empty() => n.push(*r.pick(swap(ch))),
                    1 => n.push(ch.to_ascii_uppercase()),
                    2 => n.extend([ch, ch]),
                    _ => n.push(ch),
                }
                if r.next(12) == 0 {
                    n.push(*r.pick(&[' ', '_', '-', '.', '2']));
                }
            }
            if r.next(3) == 0 {
                n.push_str(r.pick(&["", " ", "_", "42"]));
            }
            if r.next(4) == 0 {
                push_number(r, &mut n, None);
            }
        }
        n.truncate(24);
        names.push(n);
    }
    for _ in 0..5_000 {
        names.push((0..r.next(25)).map(|_| char::from(32 + r.next(95) as u8)).collect());
    }
}

/// a number of 1 to 4 digits, ASCII or (with alts) now and then styled ones
fn push_number(r: &mut Rnd, n: &mut String, alts: Option<&HashMap<char, Vec<char>>>) {
    for _ in 0..1 + r.next(4) {
        let d = char::from(b'0' + r.next(10) as u8);
        match alts.and_then(|a| a.get(&d)) {
            Some(styled) if r.next(3) == 0 => n.push(*r.pick(styled)),
            _ => n.push(d),
        }
    }
}

/// The pieces again, through other scripts and styles: letters as their lookalikes, styled or accented, marks after
/// them, separators and format characters between them, emoji and other characters, runs, capitals, numbers.
fn unicode_names(r: &mut Rnd, pieces: &[&str], names: &mut Vec<String>) {
    let (alts, marks, separators, others) = (alternatives(), marks(), separators(), others());
    let alt = |r: &mut Rnd, ch: char| alts.get(&ch).map(|a| *r.pick(a));
    for _ in 0..30_000 {
        let mut n = String::new();
        for _ in 0..1 + r.next(3) {
            for ch in r.pick(pieces).chars() {
                let ch = if r.next(5) == 0 { ch.to_ascii_uppercase() } else { ch };
                match r.next(10) {
                    0..4 => n.push(alt(r, ch).unwrap_or(ch)),
                    4 if !swap(ch).is_empty() => {
                        let sign = *r.pick(swap(ch));
                        n.push(alt(r, sign).filter(|_| r.next(2) == 0).unwrap_or(sign));
                    }
                    5 => n.extend([ch, alt(r, ch).unwrap_or(ch)]),
                    _ => n.push(ch),
                }
                match r.next(25) {
                    0 | 1 => n.push(*r.pick(&marks)),
                    2 => n.push(*r.pick(&separators)),
                    3 => n.push(*r.pick(&others)),
                    _ => {}
                }
            }
            match r.next(4) {
                0 => n.push(*r.pick(&separators)),
                1 => push_number(r, &mut n, Some(&alts)),
                _ => {}
            }
        }
        if r.next(10) == 0 {
            n.insert(0, *r.pick(&others));
        }
        names.push(n);
    }
    // plain noise of all those
    let all: Vec<char> = alts.values().flatten().chain(&marks).chain(&separators).chain(&others).copied().collect();
    for _ in 0..3_000 {
        names.push((0..r.next(30)).map(|_| *r.pick(&all)).collect());
    }
}

/// Names longer than the 255 letters names.mjs reads, a word of the lists somewhere in them.
fn long_names(r: &mut Rnd, pieces: &[&str], names: &mut Vec<String>) {
    for _ in 0..2_000 {
        let mut n: String = (0..240 + r.next(40)).map(|_| *r.pick(&['x', 'y', 'X', ' ', '_', '4'])).collect();
        let at = r.next(n.len() + 1);
        n.insert_str(at, r.pick(pieces));
        names.push(n);
    }
}

/// Every code point of the planes in use (and every 97th of the rest) in a few places of a name, so that NFKD, the
/// marks and the lookalikes are the same for all of them: one that is gone, one that is no letter, a small letter
/// and a capital each answer otherwise for "Dani?99GS", "x?Fag" and "f?g"; one this port reads with letters or
/// digits in also stands for each letter of the lists' words.
fn sweep(names: &mut Vec<String>) {
    let words: &[&str] = &[
        "f?g",
        "i?ne",
        "?oon",
        "?yke",
        "ibn?",
        "?ag",
        "fa?",
        "c?ink",
        "k?ke",
        "?igaboo",
        "?ike",
        "porch?onkey",
        "chi?k",
        "g?ok",
        "s?ic",
        "ni?q",
        "?etard",
        "?pic",
        "?ranny",
        "j?nglebunny",
        "?etback",
        "d?ke",
    ];
    for cp in 0x80..=0x10ffff_u32 {
        let in_use = cp <= 0x3_ffff || (0xe_0000..=0xe_ffff).contains(&cp);
        let Some(ch) = char::from_u32(cp).filter(|_| in_use || cp % 97 == 0) else { continue };
        let c = ch.to_string();
        for t in ["Dani?99GS", "x?Fag", "f?g"] {
            names.push(t.replace('?', &c));
        }
        if read(&c).iter().any(|&x| letter(x).is_some() || x.is_ascii_digit()) {
            names.extend(words.iter().map(|t| t.replace('?', &c)));
        }
    }
}

fn corpus() -> Vec<String> {
    let mut names: Vec<String> =
        ANYWHERE.iter().chain(WHOLE).chain(BLOCKED).chain(UNICODE).chain(ALLOWED).chain(NEAR).map(|n| n.to_string()).collect();
    names.extend(["xfagx", "x_fag_x", "xFagX"].map(String::from));
    let (mut r, pieces) = (Rnd(7), pieces());
    ascii_names(&mut r, &pieces, &mut names);
    unicode_names(&mut r, &pieces, &mut names);
    long_names(&mut r, &pieces, &mut names);
    sweep(&mut names);
    names
}

/// the node script: blockedName of each name of the corpus, names.mjs's lists, and its lookalikes (not exported:
/// read from its source)
const SCRIPT: &str = r#"
import fs from 'node:fs';
import { pathToFileURL } from 'node:url';
const [names, corpus] = process.argv.slice(2);
const { SLURS, EDGE_OK, PID_STEMS, blockedName } = await import(pathToFileURL(names).href);
let blocked = '';
for (const n of JSON.parse(fs.readFileSync(corpus, 'utf8'))) blocked += blockedName(n) ? '1' : '0';
const m = /const LOOKALIKE = (\{[\s\S]*?\});/.exec(fs.readFileSync(names, 'utf8'));
const lookalike = m && Object.entries(Function(`return (${m[1]})`)()).map(([k, v]) => [k.codePointAt(0), v.codePointAt(0)]);
process.stdout.write(JSON.stringify({ blocked, slurs: SLURS, edgeOk: EDGE_OK, stems: PID_STEMS, lookalike }));
"#;

/// a folder of its own under the system's temp, gone with it
struct TempDir(PathBuf);

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn strings(v: &serde_json::Value) -> Vec<&str> {
    v.as_array().expect("an array").iter().map(|s| s.as_str().expect("a string")).collect()
}

#[test]
fn answers_as_names_mjs() {
    let names_mjs = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../src/app/names.mjs");
    assert!(names_mjs.exists(), "no {}", names_mjs.display());
    let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_nanos();
    let dir = TempDir(std::env::temp_dir().join(format!("nfsu2-names-{}-{nanos}", std::process::id())));
    std::fs::create_dir_all(&dir.0).expect("a temp folder");
    let names = corpus();
    let (script, input) = (dir.0.join("check.mjs"), dir.0.join("names.json"));
    std::fs::write(&script, SCRIPT).expect("the script");
    std::fs::write(&input, serde_json::to_string(&names).expect("JSON")).expect("the corpus");
    let out = match Command::new("node").arg(&script).arg(&names_mjs).arg(&input).output() {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            eprintln!("answers_as_names_mjs skipped: no node ({e})");
            return;
        }
        r => r.expect("node runs"),
    };
    assert!(out.status.success(), "node failed: {}", String::from_utf8_lossy(&out.stderr));
    let js: serde_json::Value = serde_json::from_slice(&out.stdout).expect("node's JSON");

    assert_eq!(strings(&js["slurs"]["anywhere"]), ANYWHERE, "the anywhere list");
    assert_eq!(strings(&js["slurs"]["whole"]), WHOLE, "the whole list");
    assert_eq!(strings(&js["slurs"]["edge"]), EDGE, "the edge list");
    assert_eq!(strings(&js["edgeOk"]["start"]), EDGE_OK_START, "the edge list's words a part may start with");
    assert_eq!(strings(&js["edgeOk"]["end"]), EDGE_OK_END, "the edge list's words a part may end with");
    assert_eq!(strings(&js["stems"]), PID_STEMS, "the English words' starts before a pid word");

    let pairs = js["lookalike"].as_array().expect("names.mjs's LOOKALIKE table, read from its source");
    let char_of = |v: &serde_json::Value| v.as_u64().and_then(|c| char::from_u32(c as u32)).expect("a code point");
    for pair in pairs {
        let (k, v) = (char_of(&pair[0]), char_of(&pair[1]));
        assert_eq!(lookalike(k), v, "the lookalike of U+{:04X}", k as u32);
    }
    let ours = (0..=0x10ffff).filter_map(char::from_u32).filter(|&c| lookalike(c) != c).count();
    assert_eq!(ours, pairs.len(), "lookalikes names.mjs does not have");

    let answers = js["blocked"].as_str().expect("the answers").as_bytes();
    assert_eq!(answers.len(), names.len());
    let (mut blocked, mut unicode, mut diff) = (0, 0, Vec::new());
    for (name, &a) in names.iter().zip(answers) {
        let ours = blocked_name(name);
        blocked += usize::from(ours);
        unicode += usize::from(!name.is_ascii());
        if ours != (a == b'1') {
            let cps: Vec<String> = name.chars().map(|c| format!("{:x}", c as u32)).collect();
            diff.push(format!("{name:?} [{}]: names.mjs {}, here {ours}", cps.join(" "), a == b'1'));
        }
    }
    eprintln!("{} names ({unicode} past ASCII), {blocked} blocked, {} answered otherwise", names.len(), diff.len());
    assert!(diff.is_empty(), "names the two read differently:\n{}", diff[..diff.len().min(40)].join("\n"));
    assert!(blocked > 10_000 && blocked < names.len() - 10_000, "both answers among the names ({blocked} blocked)");
}
