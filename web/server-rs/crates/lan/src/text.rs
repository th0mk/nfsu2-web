//! Names, addresses, tokens and the JSON of the control messages, read the way JavaScript reads them (as the Node
//! server did: a client's hello means to the switch what it meant there).

use nfsu2_util::js;
use serde_json::Value;
use sha2::{Digest, Sha256};
use unicode_general_category::{GeneralCategory, get_general_category};
use unicode_normalization::UnicodeNormalization;


/// a display name: printable, no bidi or zero-width tricks (control and format characters removed), at most 32
/// characters, none that reads as a slur (nfsu2-names, the rules of web/src/app/names.mjs): "player" then
pub fn clean_name(s: &str) -> String {
    // NFC; each run of white space (JavaScript's \s) one space; control and format characters (Cc, Cf) out; each run
    // of spaces one space; trimmed
    let nfc = nfc_as_node(s);
    let spaced = js::collapse_space(&nfc, " ");
    let mut t = String::with_capacity(spaced.len());
    let mut space = false;
    for c in spaced.chars() {
        if matches!(get_general_category(c), GeneralCategory::Control | GeneralCategory::Format) {
            continue;
        }
        if c == ' ' {
            if !space {
                t.push(' ');
            }
            space = true;
        } else {
            t.push(c);
            space = false;
        }
    }
    // at most 32 code points (not UTF-16 units), trimmed again
    let first: String = js::trim(&t).chars().take(32).collect();
    let n = js::trim(&first);
    if !n.is_empty() && !nfsu2_names::blocked_name(n) { n.to_string() } else { "player".to_string() }
}

/// NFC as Node 24 did it for the Node server.  Its ICU is Unicode 16.0, unicode-normalization's tables are newer: a
/// character unassigned in 16.0 (unicode-general-category's version, which matches Node's) is to Node a starter that
/// decomposes into nothing and composes with nothing, so it stays where it is and the text on either side is
/// normalized on its own (no reordering or composing reaches across such a starter).  Without this, combining marks
/// of Unicode 17 were moved in front of older ones (nfsu2-names reads names with the same hold-back).
fn nfc_as_node(s: &str) -> String {
    if s.is_ascii() {
        return s.to_string();
    }
    let unknown = |c: char| !c.is_ascii() && get_general_category(c) == GeneralCategory::Unassigned;
    let mut out = String::with_capacity(s.len());
    for piece in s.split_inclusive(unknown) {
        match piece.chars().next_back() {
            Some(c) if unknown(c) => {
                out.extend(piece[..piece.len() - c.len_utf8()].nfc());
                out.push(c);
            }
            _ => out.extend(piece.nfc()),
        }
    }
    out
}

/// an address the switch hands out ("10.77.x.y" in 10.77.0-23.x of 10.77.0.0/19, host byte 2-251), else 0
pub fn vip_num(s: &str) -> u32 {
    // /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/, each part at most 255
    let mut v: u32 = 0;
    let mut parts = 0;
    for part in s.split('.') {
        let b = part.as_bytes();
        if parts == 4 || b.is_empty() || b.len() > 3 || !b.iter().all(u8::is_ascii_digit) {
            return 0;
        }
        let x: u32 = b.iter().fold(0, |a, d| a * 10 + (d - b'0') as u32);
        if x > 255 {
            return 0;
        }
        v = (v << 8) | x;
        parts += 1;
    }
    if parts != 4 {
        return 0;
    }
    if crate::slot(v).is_some() { v } else { 0 }
}

/// JS `String(x ?? '')` of a parsed JSON value
pub(crate) fn js_string(v: Option<&Value>) -> String {
    match v {
        None | Some(Value::Null) => String::new(),
        Some(v) => to_js_string(v),
    }
}

fn to_js_string(v: &Value) -> String {
    match v {
        Value::Null => "null".into(),
        Value::Bool(b) => b.to_string(),
        Value::Number(n) => match (n.as_i64(), n.as_u64()) {
            (Some(i), _) if i.unsigned_abs() < 1 << 53 => i.to_string(),
            (_, Some(u)) if u < 1 << 53 => u.to_string(),
            _ => js::number_to_string(n.as_f64().unwrap_or(f64::NAN)),
        },
        Value::String(s) => s.clone(),
        // Array#toString: join(','), with null as ''
        Value::Array(a) => a.iter().map(|x| if x.is_null() { String::new() } else { to_js_string(x) }).collect::<Vec<_>>().join(","),
        Value::Object(_) => "[object Object]".into(),
    }
}

/// JS `+s` of a string (the impairment's numbers): its whitespace trimmed, "" is 0, 0x/0o/0b integers, a decimal
pub(crate) fn to_number(s: &str) -> f64 {
    let t = js::trim(s);
    if t.is_empty() {
        return 0.0;
    }
    for (p, radix) in [("0x", 16), ("0X", 16), ("0o", 8), ("0O", 8), ("0b", 2), ("0B", 2)] {
        if let Some(d) = t.strip_prefix(p) {
            if d.is_empty() || !d.chars().all(|c| c.is_digit(radix)) {
                return f64::NAN;
            }
            return d.chars().fold(0.0, |a, c| a * radix as f64 + c.to_digit(radix).unwrap_or(0) as f64);
        }
    }
    match t {
        "Infinity" | "+Infinity" => return f64::INFINITY,
        "-Infinity" => return f64::NEG_INFINITY,
        _ => {}
    }
    // JS's StrDecimalLiteral: digits, one dot, an exponent; Rust's parser also takes "inf", "nan" and the like
    let ok = t.bytes().all(|b| b.is_ascii_digit() || matches!(b, b'.' | b'e' | b'E' | b'+' | b'-'));
    if !ok {
        return f64::NAN;
    }
    t.parse().unwrap_or(f64::NAN)
}

/// a new token (128 bits, base64url: 22 characters) that only its client sees
pub(crate) fn new_token() -> String {
    let mut b = [0u8; 16];
    nfsu2_util::rand::fill(&mut b);
    js::base64url(&b)
}

pub(crate) fn token_hash(t: &str) -> [u8; 32] {
    Sha256::digest(t.as_bytes()).into()
}

/// token (from a client) matches hash (of the token the switch gave), compared in constant time; one longer than 64
/// (UTF-16 units) never does
pub(crate) fn token_fits(token: &str, hash: &[u8; 32]) -> bool {
    if js::utf16_len(token) > 64 {
        return false;
    }
    let h = token_hash(token);
    h.iter().zip(hash).fold(0u8, |a, (x, y)| a | (x ^ y)) == 0
}

/// JSON.parse of a control message.  JS takes a lone surrogate escape ("\ud800") in a string; serde_json does not:
/// it is read as U+FFFD instead (what its UTF-8 is in Node, where the name and the token's hash see it so).
pub(crate) fn parse_json(b: &[u8]) -> Option<Value> {
    match serde_json::from_slice(b) {
        Ok(v) => Some(v),
        Err(_) => {
            let s = std::str::from_utf8(b).ok()?;
            let fixed = lone_surrogates(s)?;
            serde_json::from_str(&fixed).ok()
        }
    }
}

/// the text with every lone surrogate escape as a replacement character's escape, or None when it has none
fn lone_surrogates(s: &str) -> Option<String> {
    let b = s.as_bytes();
    let hex = |i: usize| -> Option<u16> {
        let h = s.get(i..i + 4)?;
        if !h.bytes().all(|c| c.is_ascii_hexdigit()) {
            return None;
        }
        u16::from_str_radix(h, 16).ok()
    };
    let mut out = String::with_capacity(s.len());
    let mut changed = false;
    let mut i = 0;
    let mut last = 0;
    while i < b.len() {
        if b[i] != b'\\' {
            i += 1;
            continue;
        }
        if b.get(i + 1) != Some(&b'u') {
            i += 2;
            continue;
        }
        let Some(u) = hex(i + 2) else {
            i += 2;
            continue;
        };
        if (0xD800..0xDC00).contains(&u) {
            let low = (b.get(i + 6) == Some(&b'\\') && b.get(i + 7) == Some(&b'u')).then(|| hex(i + 8)).flatten();
            if low.is_some_and(|l| (0xDC00..0xE000).contains(&l)) {
                i += 12;
                continue;
            }
        } else if !(0xDC00..0xE000).contains(&u) {
            i += 6;
            continue;
        }
        out.push_str(&s[last..i]);
        out.push_str("\\ufffd");
        i += 6;
        last = i;
        changed = true;
    }
    if !changed {
        return None;
    }
    out.push_str(&s[last..]);
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    // the answers of the Node server's cleanName for the same names
    #[test]
    fn names_as_the_switch_shows_them() {
        let y40 = "y".repeat(40);
        let a31 = "a".repeat(31);
        let a31b = format!("{a31}  b");
        for (name, shown) in [
            // a name that reads as a slur is "player" (nfsu2-names); one that only looks like one stays
            ("  N1gg4  ", "player"),
            ("Mustafa GS", "Mustafa GS"),
            // nothing left: "player"
            ("", "player"),
            ("   ", "player"),
            ("\u{AD}", "player"),
            // white space (JavaScript's \s) one space, trimmed
            ("  Bob  ", "Bob"),
            ("a\t\n b", "a b"),
            ("\u{3000}x\u{A0}y\u{FEFF}", "x y"),
            // control and format characters (Cc, Cf) out: zero-width, bidi, tags
            ("a\u{0}\u{7}b\u{7F}", "ab"),
            ("a\u{85}b", "ab"),
            ("a\u{200B}b\u{2060}c", "abc"),
            ("\u{202E}evil\u{202C}", "evil"),
            ("x\u{180E}y", "xy"),
            ("\u{E0041}tag", "tag"),
            // NFC
            ("e\u{301}", "\u{E9}"),
            ("A\u{30A}", "\u{C5}"),
            ("\u{212B}", "\u{C5}"),
            ("a\u{316}\u{301}", "\u{E1}\u{316}"),
            // a mark newer than Node's Unicode (16.0) stays where it is: nothing reordered or composed across it
            ("e\u{301}\u{1E6E3}\u{316}", "\u{E9}\u{1E6E3}\u{316}"),
            ("e\u{301}\u{10EF5}\u{316}", "\u{E9}\u{10EF5}\u{316}"),
            ("\u{1E6E3}\u{301}x", "\u{1E6E3}\u{301}x"),
            // emoji stay
            ("\u{1F600}\u{1F1E6}\u{1F1E8}", "\u{1F600}\u{1F1E6}\u{1F1E8}"),
            // at most 32 code points, trimmed again
            (y40.as_str(), &y40[..32]),
            (a31b.as_str(), a31.as_str()),
        ] {
            assert_eq!(clean_name(name), shown, "{name:?}");
        }
    }

    #[test]
    fn addresses_as_vip_num_reads_them() {
        assert_eq!(vip_num("10.77.0.2"), 0x0A4D_0002);
        assert_eq!(vip_num("10.77.7.251"), 0x0A4D_07FB);
        assert_eq!(vip_num("10.77.15.251"), 0x0A4D_0FFB);
        assert_eq!(vip_num("10.77.19.251"), 0x0A4D_13FB);
        assert_eq!(vip_num("10.77.23.251"), 0x0A4D_17FB);
        for s in [
            "10.77.0.1",
            "10.77.0.252",
            "10.77.0.255",
            "10.77.24.2",
            "10.77.31.2",
            "10.77.32.2",
            "192.168.1.20",
            "10.77.0.02.1",
            "10.77.0",
            "",
            "10.77.0.2 ",
            " 10.77.0.2",
            "10.77.0.2\n",
            "10.77.0.0002",
            "10.77.256.2",
            "1e1.77.0.2",
            "10.77.0.+2",
            "x",
        ] {
            assert_eq!(vip_num(s), 0, "{s:?}");
        }
        assert_eq!(vip_num("010.077.000.002"), 0x0A4D_0002);
    }

    #[test]
    fn strings_as_js_makes_them() {
        assert_eq!(js_string(None), "");
        assert_eq!(js_string(Some(&json!(null))), "");
        assert_eq!(js_string(Some(&json!(["10.77.0.2"]))), "10.77.0.2");
        assert_eq!(js_string(Some(&json!([1, null, [2, 3]]))), "1,,2,3");
        assert_eq!(js_string(Some(&json!({"a": 1}))), "[object Object]");
        assert_eq!(js_string(Some(&json!(1.5))), "1.5");
        assert_eq!(js_string(Some(&json!(true))), "true");
    }

    #[test]
    fn numbers_as_js_reads_them() {
        assert_eq!(to_number("80"), 80.0);
        assert_eq!(to_number(" 1e3 "), 1000.0);
        assert_eq!(to_number(""), 0.0);
        assert_eq!(to_number("0x10"), 16.0);
        assert!(to_number("inf").is_nan());
        assert!(to_number("12px").is_nan());
        assert_eq!(to_number("-2.5"), -2.5);
    }

    #[test]
    fn tokens_fit_only_their_hash() {
        let t = new_token();
        assert_eq!(t.len(), 22);
        let h = token_hash(&t);
        assert!(token_fits(&t, &h));
        assert!(!token_fits("x", &h));
        assert!(!token_fits(&"a".repeat(65), &token_hash(&"a".repeat(65))));
        assert!(token_fits(&"a".repeat(64), &token_hash(&"a".repeat(64))));
    }

    #[test]
    fn lone_surrogates_read_as_replacement_characters() {
        assert_eq!(parse_json(br#"{"name":"a\ud800b"}"#), Some(json!({"name": "a\u{FFFD}b"})));
        assert_eq!(parse_json(br#"{"name":"\udc00"}"#), Some(json!({"name": "\u{FFFD}"})));
        assert_eq!(parse_json(br#"{"name":"\ud83d\ude00"}"#), Some(json!({"name": "\u{1F600}"})));
        assert_eq!(parse_json(br#"{"name":"\\ud800"}"#), Some(json!({"name": "\\ud800"})));
        assert_eq!(parse_json(b"{nope"), None);
    }
}
