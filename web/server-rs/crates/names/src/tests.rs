//! The names.mjs cases of web/test/names.test.mjs (its first test).  What else reads names with this crate is tested
//! where it is: the switch's "player" for such a name in nfsu2-lan (text.rs's tests), the Online results dropping the
//! ones kept under one in nfsu2-online (stats.rs's tests).  runtime/names.h (its lists, its answers for 25,000 names)
//! is the game's C, checked against names.mjs by names.test.mjs's other two tests.

use super::*;

/// the ways round the lists: digits and signs for letters, gaps, repeats, case, plurals, other scripts
pub(crate) const BLOCKED: &[&str] = &[
    "nigger",
    "N1gg3r",
    "n i g g e r",
    "n.i.g.g.a",
    "NiQQa",
    "nlgga",
    "n!gga",
    "niiiigggaaa",
    "xXn1gg4Xx",
    "FAG",
    "f4g",
    "F A G",
    "f4g_42",
    "XxFagXx",
    "fags",
    "Faaag",
    "faggot",
    "f@gg0t",
    "fa66ot",
    "tr4nny",
    "Trannie",
    "r3tard",
    "ibne",
    "iBnE_34",
    "wetback",
    "beaner",
    "Ch1nk",
    "g00k",
    "sp1cs",
    "paki",
    "k1ke",
    "dyke",
    "c00n",
    "XxCoonXx",
    "towelhead",
    "sandnigger",
    "junglebunny",
    "ni99a",
    "NI99A",
    "nigga99",
    "Fag42",
    "Big1Fag",
    "Mr5Coon",
    "trannies",
    "Dykes",
    "Van_Fag",
    // the other offensive names (the live server's, 2026-10-26), 1488 in a name
    "Hitler",
    "Hitler3858",
    "adolfhitler",
    "AdolfH",
    "H1tl3r",
    "gitler",
    "Himmler",
    "zigHael",
    "NAZZI",
    "JewsHunter",
    "nigahunter",
    "Niga",
    "NigaBoi",
    "Nig4",
    "MEGANIGA",
    "KneeGro",
    "CzarnuchJebany",
    "CHURKA",
    "xoxol",
    "pidor",
    "PIDARAZ",
    "vladpidr",
    "cwel666",
    "gigacwel",
    "Mongoloid",
    "TALIBAN",
    "Et_Binladen",
    "samrapist",
    "RapeAddict",
    "ASphaltMolester",
    "1488",
    "Sam1488",
    "kkk1488",
    "Sam14881488",
];
/// 'ñigga', 'nіgga' (Cyrillic і), fullwidth, 'ΝΙGGΑ' (Greek Ν Ι Α), combining low lines, circled, 'fаg' (Cyrillic а),
/// 'Ηitler' (Greek Η), fullwidth, fullwidth digits
pub(crate) const UNICODE: &[&str] = &[
    "\u{f1}igga",
    "n\u{456}gga",
    "\u{ff4e}\u{ff49}\u{ff47}\u{ff47}\u{ff41}",
    "\u{39d}\u{399}GG\u{391}",
    "n\u{332}i\u{332}g\u{332}g\u{332}a\u{332}",
    "\u{24d5}\u{24d0}\u{24d6}",
    "f\u{430}g",
    "\u{397}itler",
    "\u{ff28}\u{ff49}\u{ff54}\u{ff4c}\u{ff45}\u{ff52}",
    "Lander\u{ff11}\u{ff14}\u{ff18}\u{ff18}",
];
/// other words with those letters in them, the player names this server sees most (Turkish) among them; then a
/// name and a number: a year, two digits, the digits the Online mode adds to a taken name (nfsu2-online `free_variant`)
pub(crate) const ALLOWED: &[&str] = &[
    "Mustafa69",
    "MustafaGS",
    "Mustafa_GS",
    "Mustafa Gamer",
    "mustafagamer",
    "Rafa96",
    "Vefa",
    "Safa",
    "Pakize",
    "Nigar",
    "Niger",
    "Nigeria",
    "Nigerian",
    "Nigel",
    "Raccoon",
    "Tycoon",
    "Cocoon",
    "spicy",
    "Spice",
    "despicable",
    "Pakistan",
    "Fagundes",
    "Bigger",
    "Trigger",
    "Digger",
    "Knight",
    "Chinook",
    "Scunthorpe",
    "Cooper",
    "Ahmet35",
    "Emre_34",
    "G\u{f6}khan",
    "\u{130}brahim",
    "Thom",
    "Player",
    "NAME",
    "OLHOST",
    "Speedy",
    "",
    "Hakan1995",
    "Furkan1999",
    "Ozan_1996",
    "Can 1969",
    "Kaan1998",
    "Dani99",
    "Toni69",
    "Dani99GS",
    "Ozan1996GS",
    "Hakan1993TR",
    "Dani9912",
    "Strannik",
    "VanDyke",
    "Van Dyke",
    // the live names with the new words' letters in them, other numbers with 88
    "Spider",
    "MR.SPIDER",
    "Shinigami",
    "Zuniga",
    "Vlado",
    "Ataliba_Neto",
    "Grapes",
    "Trapezio",
    "Adolf",
    "Nazim",
    "Nazif",
    "Nygaard",
    "Mongol",
    "Churkin",
    "jacob88",
    "Jane8848",
    "Ahmet1448",
    // English words that end in "pid" before an r or a
    "RapidRacer",
    "rapid racer",
    "CupidRacer",
    "Stupidrift",
    "StupidArmy",
    "TepidRide",
];

// names.mjs: the lists' words, the ways round them, and not the other words
#[test]
fn lists_ways_round_and_other_words() {
    for w in ANYWHERE.iter().chain(WHOLE) {
        assert!(blocked_name(w), "{w}");
    }
    for n in BLOCKED.iter().chain(UNICODE) {
        assert!(blocked_name(n), "{n}");
    }
    for n in ALLOWED {
        assert!(!blocked_name(n), "{n}");
    }
    // a word of the whole list only as a part on its own
    assert!(!blocked_name("xfagx") && blocked_name("x_fag_x") && blocked_name("xFagX"));
}

// not in names.test.mjs: the pieces on their own

#[test]
fn letters_as_the_js_reads_them() {
    for (ch, c) in [('a', Some(b'a')), ('Z', Some(b'z')), ('L', Some(b'i')), ('l', Some(b'i')), ('|', Some(b'i'))] {
        assert_eq!(letter(ch), c, "{ch}");
    }
    for (ch, c) in [('0', b'o'), ('1', b'i'), ('!', b'i'), ('3', b'e'), ('4', b'a'), ('@', b'a'), ('5', b's')] {
        assert_eq!(letter(ch), Some(c), "{ch}");
    }
    for (ch, c) in [('$', b's'), ('7', b't'), ('+', b't'), ('6', b'g'), ('9', b'g'), ('8', b'b')] {
        assert_eq!(letter(ch), Some(c), "{ch}");
    }
    for ch in ['2', ' ', '_', '-', '.', '\u{e9}', '\u{430}', '\u{ff41}', '\u{1f600}'] {
        assert_eq!(letter(ch), None, "{ch}");
    }
    assert_eq!(WHOLE_WORDS[0].letters(), b"fag");
    assert_eq!(ANYWHERE_WORDS[14].letters(), b"jungiebunny");
    assert_eq!(ANYWHERE_WORDS[11].letters(), b"chingchong");
}

#[test]
fn read_as_nfkd_without_marks_and_lookalikes() {
    let text = |s: &str| read(s).into_iter().collect::<String>();
    assert_eq!(text("Mustafa GS"), "Mustafa GS");
    assert_eq!(text("\u{f1}i\u{332}"), "ni");
    assert_eq!(text("\u{ff4e}\u{1d41a}\u{24d0}"), "naa");
    assert_eq!(text("\u{39d}\u{430}\u{20ac}"), "Nae");
    // a keycap: the digit stays, the variation selector and the enclosing keycap are marks
    assert_eq!(text("1\u{fe0f}\u{20e3}"), "1");
    // the ligature and the fraction come apart, the emoji and the zero-width joiner stay
    assert_eq!(text("\u{fb00}\u{bd}\u{1f600}\u{200d}"), "ff1\u{2044}2\u{1f600}\u{200d}");
    // a character newer than the mark table's Unicode (16.0) stays as it is, the text around it read as always
    assert_eq!(text("\u{e9}\u{1dfd5}\u{e9}"), "e\u{1dfd5}e");
}

#[test]
fn numbers_and_letters() {
    // three or more digits are a number, and a split
    assert!(blocked_name("fag123") && !blocked_name("ni999a") && !blocked_name("ni9999a"));
    // two that end a part, or before a capital after a small letter, are a number; between small letters, letters
    assert!(!blocked_name("Dani99") && !blocked_name("Dani99GS") && blocked_name("ni99a"));
    assert!(!blocked_name("Ni99A") && blocked_name("NI99A"));
    // one digit is always read as a letter, or for 2 as none: a split, the name's letters still read together
    assert!(blocked_name("f4g") && blocked_name("f2ag") && blocked_name("fa2g2") && !blocked_name("fa2g2x"));
    // 1488 anywhere, in any run of digits, past the 255 letters too; not its digits apart
    assert!(blocked_name("x1488x") && blocked_name("2281488") && blocked_name(&("x".repeat(300) + "1488")));
    assert!(!blocked_name("14 88") && !blocked_name("1448") && !blocked_name("14_88"));
}

#[test]
fn parts_and_runs() {
    assert!(blocked_name("niiiiggggaaaa") && !blocked_name("Niger") && !blocked_name("nige"));
    assert!(blocked_name("XxCoonXx") && !blocked_name("xxcoonxx") && blocked_name("xx_coon_xx"));
    assert!(blocked_name("F A G") && blocked_name("f.a.g.s") && !blocked_name("f a g x"));
    assert!(!blocked_name("Van Dyke") && !blocked_name("VanDyke") && blocked_name("Dyke") && blocked_name("X Dyke"));
    assert!(blocked_name("Van Fag") && blocked_name("Vanfag Dyke"));
}

#[test]
fn a_pid_word_after_a_vowel_is_english() {
    assert!(!blocked_name("RapidRacer") && !blocked_name("xStupidorx") && !blocked_name("Insipidar"));
    assert!(blocked_name("vladpidr") && blocked_name("Pidr228") && blocked_name("xpidrx") && blocked_name("rapid_pidr"));
}

// the lobby's chat: a word at a time, two words together for the anywhere list, one-letter words in a row
#[test]
fn chat_lines() {
    let slur = |l: &str| chat_slur(l).map(str::to_string);
    for (line, span) in [
        ("you nigger", "nigger"),
        ("Sranie> nate higgers", "nate higgers"),
        ("nig ger lol", "nig ger"),
        ("SIEG HEIL", "SIEG HEIL"),
        ("n i g g e r", "n i g g e r"),
        ("so F A G", "F A G"),
        ("u r a f a g", "f a g"),
        ("f.a.g.s", "f.a.g.s"),
        ("ur a fag!!", "fag"),
        ("you n!gger", "n!gger"),
        ("N1GG4 pls", "N1GG4"),
        ("go 1488", "1488"),
        ("porch monkey", "porch monkey"),
        ("van Dyke_fag", "Dyke_fag"),
        ("van dyke-nigger", "dyke-nigger"),
    ] {
        assert_eq!(slur(line).as_deref(), Some(span), "{line}");
    }
    // swearing stays, and so do the words across words that the letters all together would read
    for line in [
        "merci kurwa",
        "SEN? MERC? AMK",
        "HACI> DOM TORETTO H?S FATHER",
        "rapid race",
        "stupid race",
        "go ok",
        "go,ok",
        "fa g",
        "Van Dyke",
        "a b c d e",
        "the spice must flow",
        "hi! I'm Nigel from Nigeria",
        "lap 1:48.8",
        "",
        "   ",
    ] {
        assert_eq!(slur(line), None, "{line}");
    }
    // the span is the line's own text
    let line = "a\u{a0}nate  higgers b";
    assert_eq!(chat_slur(line), Some("nate  higgers"));
}

#[test]
fn letters_past_255_are_not_read() {
    let x = |n: usize| "x".repeat(n);
    assert!(blocked_name(&(x(250) + "nigga")));
    assert!(!blocked_name(&(x(252) + "nigga")));
    // a part of its own is read from its start, whatever came before
    assert!(blocked_name(&(x(300) + "_fag")) && blocked_name(&(x(300) + " " + &x(300) + "_fag")));
    assert!(!blocked_name(&(x(300) + "fag")));
}
