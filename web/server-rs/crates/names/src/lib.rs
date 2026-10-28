//! A player's name that reads as a slur: the rules of web/src/app/names.mjs (the page's), ported.  The server refuses
//! one for the Online mode (an account or persona; the Online results drop the ones kept under one) and the switch's
//! peer list shows "player" for one (nfsu2-lan's `clean_name`); the page's settings refuse one as "Your name".
//! runtime/names.h is the same lists and the same matching for the game (the names over the cars in free roam).
//!
//! A name is read as letters only: a to z in lower case, the digits and signs that stand for letters as those
//! (0 o, 1 ! | l i, 3 e, 4 @ a, 5 $ s, 7 + t, 6 9 g, 8 b), anything else left out: "N1-g g@" reads "nigga".  Digits
//! that make a number are left out too: three or more in a row, or two that end a part ("Hakan1995", "Dani99",
//! "Dani99GS" are no slur; "ni99a" is one).
//! Before that (here only: the game's names are ASCII) accents, fullwidth and styled letters go (NFKD) and the
//! Cyrillic and Greek letters that look like Latin ones read as those.  A word matches where each of its letters
//! comes at least as many times in a row as in the word ("niiiggga"; "Niger" is not "nigger").
//!  - anywhere: a word long enough that no other word has it inside it
//!  - whole: a short one that other words have inside them ("Mustafa GS", "raccoon", "spicy", "Pakize"), only as a
//!    part of the name on its own, and plural: the parts are split where a character is no letter and where a
//!    capital follows a small letter, digits and signs read as letters between them or not ("f4g_42", "XxCoonXx",
//!    "Big1Fag"); the name's letters together count as one too ("F A G").  "Dyke" after "Van" is the surname.
//!
//! The other offensive names go the same way (picked from the live server's names, 2026-10-26): Nazi ones, hate
//! aimed at a group, terror, sexual violence; and a name with 1488 in it (the neo-Nazi code).  A swear word is no
//! such name.  A "pid" word (pidor, pidar, pidr) right after a vowel is an English word's end ("RapidRacer").
//!
//! The Online lobby's chat is read with the same lists, a word at a time ([`chat_slur`]); here only.

use unicode_general_category::{GeneralCategory, get_general_category};
use unicode_normalization::UnicodeNormalization;

#[cfg(test)]
mod cross_check;
#[cfg(test)]
mod tests;

/// words that match anywhere in a name
pub const ANYWHERE: &[&str] = &[
    "nigg",
    "niqq",
    "faggot",
    "tranny",
    "trannie",
    "retard",
    "ibne",
    "wetback",
    "beaner",
    "raghead",
    "towelhead",
    "chingchong",
    "jigaboo",
    "porchmonkey",
    "junglebunny",
    "nibba",
    "nigus",
    "nygga",
    "kneegro",
    "wigga",
    "meganiga",
    "czarnuch",
    "czarnuh",
    "churka",
    "pidor",
    "pidar",
    "pidr",
    "gomosek",
    "cwel",
    "baitola",
    "maricon",
    "gotveren",
    "gotoglani",
    "mongoloid",
    "hitler",
    "gitler",
    "zitler",
    "adolfh",
    "himler",
    "fuhrer",
    "fuehrer",
    "siegheil",
    "zighael",
    "goebbels",
    "mengele",
    "mussolini",
    "jewhunt",
    "jewshunt",
    "nigahunt",
    "zenciav",
    "natehig",
    "binladen",
    "benladen",
    "rapist",
    "molester",
];
/// short words that match only as a part of the name on its own
pub const WHOLE: &[&str] = &[
    "fag", "dyke", "coon", "spic", "paki", "kike", "gook", "chink", "niga", "nyga", "ngga", "neger", "xach", "xoxol", "hohol", "ukrop",
    "pider", "pedik", "nazi", "taliban", "rape",
];
/// a name with these digits in a row in it is one too (the neo-Nazi code)
const CODE: &[char] = &['1', '4', '8', '8'];

/// The digits and signs that stand for letters, and those letters, by position.
const FROM: &[u8; 15] = b"01!|l34@5$7+698";
const TO: &[u8; 15] = b"oiiiieaassttggb";

/// names.mjs `letter(ch)` for every ASCII character, worked out once: the letter it reads as, 0 for none.  No other
/// character reads as a letter.
const LETTERS: [u8; 128] = {
    let mut table = [0; 128];
    let mut ch = 0;
    while ch < 128 {
        let low = (ch as u8).to_ascii_lowercase();
        let mut letter = if low.is_ascii_lowercase() { low } else { 0 };
        let mut i = 0;
        while i < FROM.len() {
            if FROM[i] == low {
                letter = TO[i];
            }
            i += 1;
        }
        table[ch] = letter;
        ch += 1;
    }
    table
};

/// ch as a letter (above), else None
fn letter(ch: char) -> Option<u8> {
    let c = *LETTERS.get(ch as usize)?;
    (c != 0).then_some(c)
}

/// A word of the lists as the letters a name is read as (`word` in names.mjs: "l" reads "i", so "junglebunny" is
/// looked for as "jungiebunny"), worked out at compile time.
#[derive(Clone, Copy)]
struct Word {
    letters: [u8; 16],
    len: usize,
}

impl Word {
    const fn new(w: &str) -> Word {
        let (bytes, mut letters, mut len, mut i) = (w.as_bytes(), [0; 16], 0, 0);
        while i < bytes.len() {
            // the lists are ASCII; a byte of a longer UTF-8 character reads as no letter, as that character would
            let c = if bytes[i] < 128 { LETTERS[bytes[i] as usize] } else { 0 };
            if c != 0 {
                letters[len] = c;
                len += 1;
            }
            i += 1;
        }
        Word { letters, len }
    }

    fn letters(&self) -> &[u8] {
        &self.letters[..self.len]
    }
}

const fn words<const N: usize>(list: &[&str]) -> [Word; N] {
    let mut out = [Word { letters: [0; 16], len: 0 }; N];
    let mut i = 0;
    while i < N {
        out[i] = Word::new(list[i]);
        i += 1;
    }
    out
}

const ANYWHERE_WORDS: [Word; ANYWHERE.len()] = words(ANYWHERE);
const WHOLE_WORDS: [Word; WHOLE.len()] = words(WHOLE);
const DYKE: [Word; 1] = words(&["dyke"]);

/// The anywhere words by their first letter (a to z), as bits of their index: a word is at s[i] only when s[i] is
/// its first letter, so only those are looked for there (the lobby's chat reads every line).
const ANYWHERE_FIRST: [u128; 26] = {
    assert!(ANYWHERE.len() <= 128);
    let mut t = [0; 26];
    let mut i = 0;
    while i < ANYWHERE_WORDS.len() {
        t[(ANYWHERE_WORDS[i].letters[0] - b'a') as usize] |= 1 << i;
        i += 1;
    }
    t
};

/// The Cyrillic and Greek letters that look like Latin ones read as those, and a few Latin ones NFKD keeps (dotless
/// i, L, O and D with a stroke, eng, script g), the inverted exclamation mark and the euro sign.
fn lookalike(ch: char) -> char {
    match ch {
        // Cyrillic А, Greek Α
        '\u{0410}' | '\u{0391}' => 'A',
        // Cyrillic В, Greek Β
        '\u{0412}' | '\u{0392}' => 'B',
        // Cyrillic С, Greek lunate sigma Ϲ
        '\u{0421}' | '\u{03F9}' => 'C',
        // Cyrillic Komi Ԁ, Latin Đ
        '\u{0500}' | '\u{0110}' => 'D',
        // Cyrillic Е, Greek Ε
        '\u{0415}' | '\u{0395}' => 'E',
        // Cyrillic Н, Greek Η
        '\u{041D}' | '\u{0397}' => 'H',
        // Cyrillic І, Greek Ι
        '\u{0406}' | '\u{0399}' => 'I',
        // Cyrillic Ј
        '\u{0408}' => 'J',
        // Cyrillic К, Greek Κ
        '\u{041A}' | '\u{039A}' => 'K',
        // Latin Ł
        '\u{0141}' => 'L',
        // Cyrillic М, Greek Μ
        '\u{041C}' | '\u{039C}' => 'M',
        // Greek Ν
        '\u{039D}' => 'N',
        // Cyrillic О, Greek Ο, Latin Ø
        '\u{041E}' | '\u{039F}' | '\u{00D8}' => 'O',
        // Cyrillic Р, Greek Ρ
        '\u{0420}' | '\u{03A1}' => 'P',
        // Cyrillic Ѕ
        '\u{0405}' => 'S',
        // Cyrillic Т, Greek Τ
        '\u{0422}' | '\u{03A4}' => 'T',
        // Cyrillic Х, Greek Χ
        '\u{0425}' | '\u{03A7}' => 'X',
        // Cyrillic У, Greek Υ
        '\u{0423}' | '\u{03A5}' => 'Y',
        // Greek Ζ
        '\u{0396}' => 'Z',
        // Cyrillic а, Greek α
        '\u{0430}' | '\u{03B1}' => 'a',
        // Cyrillic в, Ь, ь, Greek β
        '\u{0432}' | '\u{042C}' | '\u{044C}' | '\u{03B2}' => 'b',
        // Cyrillic с, Greek lunate sigma ϲ
        '\u{0441}' | '\u{03F2}' => 'c',
        // Cyrillic Komi ԁ, Latin đ
        '\u{0501}' | '\u{0111}' => 'd',
        // Cyrillic е, Greek ε, the euro sign
        '\u{0435}' | '\u{03B5}' | '\u{20AC}' => 'e',
        // Latin script ɡ
        '\u{0261}' => 'g',
        // Cyrillic н
        '\u{043D}' => 'h',
        // Cyrillic і, Greek ι, Latin dotless ı, the inverted exclamation mark
        '\u{0456}' | '\u{03B9}' | '\u{0131}' | '\u{00A1}' => 'i',
        // Cyrillic ј
        '\u{0458}' => 'j',
        // Cyrillic к, Greek κ
        '\u{043A}' | '\u{03BA}' => 'k',
        // Latin ł
        '\u{0142}' => 'l',
        // Cyrillic м
        '\u{043C}' => 'm',
        // Cyrillic п, Greek η, Latin ŋ
        '\u{043F}' | '\u{03B7}' | '\u{014B}' => 'n',
        // Cyrillic о, Greek ο, Latin ø
        '\u{043E}' | '\u{03BF}' | '\u{00F8}' => 'o',
        // Cyrillic р, Greek ρ
        '\u{0440}' | '\u{03C1}' => 'p',
        // Cyrillic Kurdish ԛ
        '\u{051B}' => 'q',
        // Cyrillic ѕ
        '\u{0455}' => 's',
        // Cyrillic т, Greek τ
        '\u{0442}' | '\u{03C4}' => 't',
        // Greek υ
        '\u{03C5}' => 'u',
        // Greek ν
        '\u{03BD}' => 'v',
        // Cyrillic х, Greek χ
        '\u{0445}' | '\u{03C7}' => 'x',
        // Cyrillic у
        '\u{0443}' => 'y',
        _ => ch,
    }
}

/// `\p{M}`: a mark (Mn, Mc, Me)
fn is_mark(ch: char) -> bool {
    matches!(get_general_category(ch), GeneralCategory::NonspacingMark | GeneralCategory::SpacingMark | GeneralCategory::EnclosingMark)
}

/// A character the mark table's Unicode has not assigned yet.  unicode-normalization's tables can be newer than
/// that (and than Node's ICU, which is the same Unicode, 16.0 as this is written): ICU leaves a character it does
/// not know as it is, with no decomposition and combining class 0, so it also ends a run of marks to reorder.
fn unknown(ch: char) -> bool {
    !ch.is_ascii() && get_general_category(ch) == GeneralCategory::Unassigned
}

/// The name's characters as names.mjs reads them, `[...name.normalize('NFKD').replace(/\p{M}/gu, '')]` with the
/// lookalikes as their Latin letters.
fn read(name: &str) -> Vec<char> {
    // NFKD, the marks and the lookalikes change no ASCII character
    if name.is_ascii() {
        return name.bytes().map(char::from).collect();
    }
    let mut out = Vec::with_capacity(name.len());
    for piece in name.split_inclusive(unknown) {
        let (known, new) = match piece.chars().next_back() {
            Some(ch) if unknown(ch) => (&piece[..piece.len() - ch.len_utf8()], Some(ch)),
            _ => (piece, None),
        };
        out.extend(known.nfkd().filter(|&ch| !is_mark(ch)).map(lookalike));
        out.extend(new);
    }
    out
}

/// The run of n digits at str[i] is a number, not letters: three or more, or two with no letter after them or a
/// capital after a small letter before them.
fn number(str: &[char], i: usize, n: usize) -> bool {
    if n != 2 {
        return n > 2;
    }
    let before = i.checked_sub(1).and_then(|b| str.get(b));
    match str.get(i + 2) {
        None => true,
        Some(&after) => letter(after).is_none() || (after.is_ascii_uppercase() && before.is_some_and(|b| b.is_ascii_lowercase())),
    }
}

/// The end of word w (its letters) at s[i], each letter at least as many times in a row as in w (the name's run of
/// it is taken whole); None if it is not there.
fn at(s: &[u8], mut i: usize, w: &[u8]) -> Option<usize> {
    let run = |from: &[u8], c: u8| from.iter().take_while(|&&x| x == c).count();
    let mut j = 0;
    while let Some(&c) = w.get(j) {
        let m = run(&w[j..], c);
        let k = run(s.get(i..).unwrap_or_default(), c);
        if k < m {
            return None;
        }
        i += k;
        j += m;
    }
    Some(i)
}

/// t is all one of words, or its plural
fn whole(t: &[u8], words: &[Word]) -> bool {
    words.iter().any(|w| at(t, 0, w.letters()).is_some_and(|e| t[e..].iter().all(|&c| c == b's')))
}

/// The JS keeps at most 255 letters of a name, and of a part of it: the rest of a longer one is not read.
const CAP: usize = 255;

/// Letters read so far, at most [`CAP`] of them.
struct Letters {
    buf: [u8; CAP],
    len: usize,
}

impl Letters {
    fn new() -> Letters {
        Letters { buf: [0; CAP], len: 0 }
    }

    fn push(&mut self, c: u8) {
        if self.len < CAP {
            self.buf[self.len] = c;
            self.len += 1;
        }
    }

    fn get(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}

/// A "pid" word of the anywhere list (pidor, pidar, pidr) at s[i] right after a vowel is the end of an English word:
/// rapid, cupid, stupid, tepid ("RapidRacer", "CupidRacer", "Stupidrift"; "vladpidr" is one)
fn english_pid(s: &[u8], i: usize, w: &[u8]) -> bool {
    w.starts_with(b"pid") && i > 0 && b"aeiouy".contains(&s[i - 1])
}

/// a word of the anywhere list is in s (letters)
fn anywhere(s: &[u8]) -> bool {
    (0..s.len()).any(|i| {
        let mut m = ANYWHERE_FIRST[(s[i] - b'a') as usize];
        while m != 0 {
            let w = ANYWHERE_WORDS[m.trailing_zeros() as usize].letters();
            if at(s, i, w).is_some() && !english_pid(s, i, w) {
                return true;
            }
            m &= m - 1;
        }
        false
    })
}

/// A name read: its letters, and whether each of its parts is one letter ("F A G", "f.a.g")
struct Scan {
    s: Letters,
    single: bool,
}

/// The name's letters, None when 1488 or a part of its own that is a word of the whole list is in it
fn scan(name: &str) -> Option<Scan> {
    let str = read(name);
    if str.windows(CODE.len()).any(|w| w == CODE) {
        return None;
    }
    // s: all the name's letters; t: the part being read
    let (mut s, mut t) = (Letters::new(), Letters::new());
    // the part before was "van"; the last letter (not a digit or sign read as one) was a small one
    let (mut after_van, mut low, mut single) = (false, false, true);
    let mut i = 0;
    while i <= str.len() {
        let ch = str.get(i).copied();
        let r = str[i..].iter().take_while(|c| c.is_ascii_digit()).count();
        let num = r > 0 && number(&str, i, r);
        let c = if num { None } else { ch.and_then(letter) };
        if (c.is_none() || (low && ch.is_some_and(|ch| ch.is_ascii_uppercase()))) && t.len > 0 {
            if whole(t.get(), &WHOLE_WORDS) && !(after_van && whole(t.get(), &DYKE)) {
                return None;
            }
            after_van = t.get() == b"van";
            single &= t.len == 1;
            t.len = 0;
        }
        match (c, ch) {
            (Some(c), Some(ch)) => {
                s.push(c);
                t.push(c);
                if ch.is_ascii_alphabetic() {
                    low = ch.is_ascii_lowercase();
                }
            }
            _ => low = false,
        }
        i += if num { r } else { 1 };
    }
    Some(Scan { s, single })
}

/// names.mjs `blockedName(name)`: the name reads as a slur (above)
pub fn blocked_name(name: &str) -> bool {
    scan(name).is_none_or(|n| anywhere(n.s.get()) || whole(n.s.get(), &WHOLE_WORDS))
}

/// The most one-letter words in a row read as a word of the whole list ("f a g s")
const WHOLE_RUN: usize = 8;

/// A chat line's words that read as a slur (the Online lobby's chat, nfsu2-online): the first such span of the
/// line, None for none.  The line is no name: its letters all together would read words across its words ("rapid
/// race" has "pidr" in it, "go ok" is "gook"), so it is read a word (between spaces, without the ! at its end) at a
/// time:
///  - a word as a name: 1488 or a part of the whole list in it, a word of the anywhere list in its letters; its
///    letters together only when each part is one letter ("f.a.g", not "go,ok"); "Dyke" after "van" is the surname
///  - two words in a row: a word of the anywhere list across them ("nig ger", "sieg heil", "nate higgers")
///  - one-letter words in a row: their letters, for the anywhere list ("n i g g a"), and three to eight of them for
///    the whole list ("F A G", "u r a f a g")
pub fn chat_slur(line: &str) -> Option<&str> {
    let mut words: Vec<(usize, usize)> = Vec::new();
    let mut from = None;
    for (i, ch) in line.char_indices().chain([(line.len(), ' ')]) {
        match (ch.is_whitespace(), from) {
            (true, Some(f)) => {
                // the ! at a word's end is punctuation, not an i
                let e = f + line[f..i].trim_end_matches('!').len();
                if e > f {
                    words.push((f, e));
                }
                from = None;
            }
            (false, None) => from = Some(i),
            _ => {}
        }
    }
    // each word's letters
    let mut letters: Vec<Vec<u8>> = Vec::with_capacity(words.len());
    for (k, &(a, b)) in words.iter().enumerate() {
        // a word after "van" is read with it, as the name "van ..." ("van Dyke")
        let after_van = k > 0 && letters[k - 1] == b"van";
        let (n, skip) = match scan(&line[a..b]) {
            None if after_van => (scan(&line[words[k - 1].0..b]), 3),
            n => (n, 0),
        };
        let s = match n {
            Some(n) if !anywhere(n.s.get()) && !(n.single && whole(n.s.get(), &WHOLE_WORDS)) => n.s.get()[skip..].to_vec(),
            _ => return Some(&line[a..b]),
        };
        if k > 0 && anywhere(&[letters[k - 1].as_slice(), &s].concat()) {
            return Some(&line[words[k - 1].0..b]);
        }
        letters.push(s);
    }
    // the one-letter words in a row
    let mut k = 0;
    while k < words.len() {
        let n = letters[k..].iter().take_while(|s| s.len() == 1).count();
        if n >= 3 {
            let s: Vec<u8> = letters[k..k + n].iter().map(|s| s[0]).collect();
            let span = |i: usize, j: usize| &line[words[k + i].0..words[k + j - 1].1];
            if anywhere(&s) {
                return Some(span(0, n));
            }
            for i in 0..n {
                for j in i + 3..=(i + WHOLE_RUN).min(n) {
                    if whole(&s[i..j], &WHOLE_WORDS) {
                        return Some(span(i, j));
                    }
                }
            }
        }
        k += n.max(1);
    }
    None
}
