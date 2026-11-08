// A player's name that reads as a slur.  The server (web/server-rs/crates/names, the same rules) refuses one for
// the Online mode (an account or persona; its results drop the ones kept under one) and the switch's peer list
// shows "player" for one (nfsu2-lan clean_name); the page's settings refuse one as "Your name"
// (web/src/app/settings.ts).
// runtime/names.h is the same lists and the same matching for the game (the names over the cars in free roam,
// and the Mac's settings); web/test/names.test.mjs checks that the two agree.
//
// A name is read as letters only: a to z in lower case, the digits and signs that stand for letters as those
// (0 o, 1 ! | l i, 3 e, 4 @ a, 5 $ s, 7 + t, 6 9 g, 8 b), anything else left out: "N1-g g@" reads "nigga".  Digits
// that make a number are left out too: three or more in a row, or two that end a part ("Hakan1995", "Dani99",
// "Dani99GS" are no slur; "ni99a" is one); but not right after a part that is a lone n (or ni) so far, with a
// letter after them ("N1663r", "BigN199a"; "N1995" stays a letter and a year).
// Before that (here only: the game's names are ASCII) accents, fullwidth and styled letters go (NFKD) and the
// Cyrillic and Greek letters that look like Latin ones read as those.  A word matches where each of its letters
// comes at least as many times in a row as in the word ("niiiggga"; one g is not the two of "nigger").
//  - anywhere: a word long enough that no other word has it inside it; "niger" not where "nigeri" is (Nigeria)
//  - whole: a short one that other words have inside them ("Mustafa GS", "raccoon", "spicy", "Pakize"), only as
//    a part of the name on its own, and plural: the parts are split where a character is no letter and where a
//    capital follows a small letter, digits and signs read as letters between them or not ("f4g_42", "XxCoonXx",
//    "Big1Fag"); the name's letters together count as one too ("F A G").  "Dyke" after "Van" is the surname; a
//    name of WHOLE_OK is none ("Nazli")
//  - edge: a word that starts or ends a part (or the name's letters together), plural too ("nigabro", "biggniga"),
//    but not where a part starts or ends with a word of EDGE_OK ("Nigar", "Minigame" ends no part with it,
//    "Zuniga", "Hoonigan" is "hooniga" when cut to 7 letters)
// The other offensive names go the same way (picked from the live server's names, 2026-10-26, 11-02, 11-05 and
// 11-07): Nazi ones and their nicknames, hate aimed at a group, terror, sexual violence; and a name with 1488 in it
// (the neo-Nazi code), spaces, _ or - between its digits or not ("14_88"); and hate aimed at a group as a phrase
// (HATE: a verb, a word between or none, the group).  A swear word is no such name.  A "pid" word (pidor, pidar,
// pidr) right after the start of an English word that ends in "pid" (rapid, cupid, stupid), or after a vowel when it
// runs into the next part ("TopiDrift"), is no slur ("RapidRacer"; "dapidor", "YaPidor" are).
export const SLURS = Object.freeze({
  anywhere: Object.freeze(['nigg', 'niqq', 'faggot', 'tranny', 'trannie', 'retard', 'ibne', 'wetback', 'beaner',
    'raghead', 'towelhead', 'chingchong', 'jigaboo', 'porchmonkey', 'junglebunny',
    'nibba', 'nigus', 'nygga', 'kneegro', 'wigga', 'meganiga', 'czarnuch', 'czarnuh', 'churka',
    'pidor', 'pidar', 'pidr', 'gomosek', 'cwel', 'baitola', 'maricon', 'gotveren', 'gotoglani', 'mongoloid',
    'hitler', 'gitler', 'zitler', 'adolfh', 'himler', 'fuhrer', 'fuehrer', 'siegheil', 'zighael', 'goebbel',
    'mengele', 'mussolini', 'jewhunt', 'jewshunt', 'nigahunt', 'zenciav', 'natehig', 'binladen', 'benladen',
    'rapist', 'molester',
    'niger', 'higger', 'figger', 'chigga', 'negroid', 'zencigot', 'kacap', 'rusnya', 'pideras', 'frocio', 'pretomacaco',
    'kitler', 'fitler', 'ritler', 'sitler', 'xitler', 'heitler', 'nitler', 'neonazi', 'nazist', 'uncleadolf',
    'austrianartist', 'austrianpaint', 'pintoraustria', 'zyklonb', 'kristalnacht', 'ovendodger',
    'jewkil', 'jewslay', 'jewdestroy', 'jebaczyd', 'zydzidopieca', 'osamabin', 'alqaeda', 'pedophil', 'pedofil',
    'wigger', 'niglet', 'nignog', 'nibber', 'iggernay', 'neegr', 'nigoger', 'nigrigg', 'nigler', 'nigcer', 'nigwer',
    'poofter', 'binladin', 'htler', 'hitlr', 'hitlur', 'gasthejuice', 'goyim', 'goyslop', 'rapegod', 'rapedog', 'rapeloli', 'biraped',
    'sigheil', 'seigheil', 'zigheil', 'ziegheil', 'zeigheil', 'sieghail', 'seighail', 'sighail', 'heilnaz', 'heiluncle',
    'hitl8r', 'hitteru', 'reltih', 'furher', 'austriandraw', 'ausartreject', 'wehrmacht', 'gestapo', 'waffenss',
    'dasreich', 'thirdreich', 'drittesreich', 'goering', 'meinkampf', 'auschwitz', 'holohoax', 'hakenkreuz',
    'whitepower', 'kukluxklan', 'klansman', 'jewhate', 'nojews', 'jewsdid', '4threich', 'fourthreich', 'holocaust',
    'adolfwasright', 'adolfpaint', 'adolfthepaint', 'whitepride', 'jewskil', 'yahudisik', 'natsist', 'nacist', 'nigaman',
    'nigakil', 'swastika', 'bitler']),
  whole: Object.freeze(['fag', 'dyke', 'coon', 'spic', 'paki', 'kike', 'gook', 'chink',
    'niga', 'nyga', 'ngga', 'neger', 'xach', 'xoxol', 'hohol', 'ukrop', 'pider', 'pedik', 'nazi', 'taliban', 'rape',
    'nig', 'killnig', 'fucknig', 'hatenig', 'pedo', 'goy', 'hilter', 'heilh', 'heilah', 'heilss', 'ssheil', 'heilhit',
    'hitlor', 'adolf', 'adolph', 'reich']),
  edge: Object.freeze(['niga', 'nyga', 'hitla', 'hitle', 'nazi']),
});
/** the words a part may start (start) or end (end) with that would read as an edge word there and are none:
 *  a name (Nigar, Nygaard; Nazir, Nazim, Nazli...), "Nigam..." ("NIGAMI", a game), a Japanese name (Niigaki), a
 *  surname (Zuniga, Ignazi), a word (kniga: a book; kunigas: a priest; Minigas), a 7-letter cut (Hooniga, Shiniga),
 *  Brony... */
export const EDGE_OK = Object.freeze({
  start: Object.freeze(['nigam', 'nigar', 'nigak', 'nygar',
    'nazir', 'nazim', 'nazif', 'nazik', 'nazih', 'nazib', 'nazia', 'nazio', 'nazli']),
  end: Object.freeze(['zuniga', 'kniga', 'kuniga', 'miniga', 'hooniga', 'shiniga', 'bronyga', 'manyga',
    'yanazi', 'ignazi', 'nazli']),
});
/** the names a part may be that read as a word of the whole list and are none, as they are (no more of a letter in
 *  a row, no plural): Nazli (Turkish: Nazlı), Nazilli (a town), "l" reading "i" */
export const WHOLE_OK = Object.freeze(['nazli', 'nazilli']);
/** the starts of the English words that end in "pid": rapid, cupid, stupid, tepid, vapid, insipid, lipid, limpid,
 *  sapid, intrepid, hispid */
export const PID_STEMS = Object.freeze(['ra', 'cu', 'stu', 'te', 'va', 'insi', 'li', 'lim', 'sa', 'intre', 'his']);
/** hate aimed at a group as a phrase, matched anywhere as the anywhere list's words are: a verb, one of the words
 *  that may come between or none, a group ("GasJews", "Kill_All_Jews") */
export const HATE = Object.freeze({
  verbs: Object.freeze(['gas', 'kill', 'hate', 'hunt', 'slay', 'burn', 'shoot', 'nuke', 'bomb', 'dead', 'death', 'fck',
    'fuck', 'fuk', 'destroy', 'exterminate']),
  between: Object.freeze(['s', 'the', 'all', 'allthe', 'da', 'dem', 'them', 'those', 'every', 'dirty', 'to']),
  groups: Object.freeze(['jew', 'kike', 'colored', 'coloured', 'zyd']),
});

/** License plates (plates.ts: capitals and digits, at most 8) read as names do (blockedName), and more: a plate has
 *  no small letters to split its parts by ("BIGFAG" is "BigFag") and its words run into each other, so:
 *  - edge: a word that ends the plate's letters, plural too ("BIGFAG", "KILLFAGS"), unless they end with a word of
 *    endOk (a word or name: "RACCOON", "ASPIC", "TORPEDO", König, Sehnig; one cut to 8 or misspelled: "MIDNIG(HT)",
 *    "LIGHTNIG", "RAFAG(A)"), or that starts them with a word of rests after it, plural or not ("FAGBOY", "NIGSKING")
 *  - anywhere: a word anywhere in its letters, unless a word of anywhereOk holds it ("HUNGGRY", Anggraini)
 *  - its letters as a name's, but a lone digit at its end is a number ("MUSTAFA6", "HUZAIFA9")
 *  - codes: a part (letters or digits in a row) that is one of alone ("KKK", "KKK77"; not "KKKK": laughter), or one of
 *    marks next to one of numbers ("HH88", "88SS", "HEIL88"; not 14: "SS14" is a car's)
 *  Picked from the live server's names read as plates (capitals and digits, the first 8), 2026-11-08. */
export const PLATE = Object.freeze({
  edge: Object.freeze(['fag', 'coon', 'spic', 'paki', 'kike', 'kyke', 'gook', 'chink', 'dyke', 'nig', 'nigr', 'neger',
    'pedo', 'rtard']),
  endOk: Object.freeze(['rafag', 'tafag', 'racoon', 'cocoon', 'tycoon', 'aspic', 'ospic', 'uspic', 'papaki', 'ipaki',
    'eepaki', 'klike', 'akike', 'ekike', 'ikike', 'okyke', 'ergook', 'achink', 'uchink', 'vandyke', 'oldyke', 'odyke',
    'knig', 'midnig', 'ghtnig', 'sehnig', 'anig', 'enig', 'inig', 'onig', 'unig', 'reneger', 'torpedo', 'speedo', 'ipedo',
    'ppedo']),
  rests: Object.freeze(['boy', 'boi', 'lord', 'king', 'kid', 'kill', 'killer', 'slayer', 'hunter', 'lover', 'life']),
  anywhere: Object.freeze(['nggr', 'jewgas', 'jewsgas', 'h8jew']),
  anywhereOk: Object.freeze(['anggr', 'unggr']),
  alone: Object.freeze(['KKK']),
  marks: Object.freeze(['HH', 'SS', 'WP', 'HEIL', 'SIEG']),
  numbers: Object.freeze(['88']),
});

const LOOKALIKE = {
  А: 'A', а: 'a', В: 'B', в: 'b', Е: 'E', е: 'e', К: 'K', к: 'k', М: 'M', м: 'm', Н: 'H', н: 'h', О: 'O', о: 'o',
  Р: 'P', р: 'p', С: 'C', с: 'c', Т: 'T', т: 't', У: 'Y', у: 'y', Х: 'X', х: 'x', І: 'I', і: 'i', Ј: 'J', ј: 'j',
  Ѕ: 'S', ѕ: 's', Ԁ: 'D', ԁ: 'd', п: 'n', Ь: 'b', ь: 'b', ԛ: 'q',
  Α: 'A', α: 'a', Β: 'B', β: 'b', Ε: 'E', ε: 'e', Ζ: 'Z', Η: 'H', η: 'n', Ι: 'I', ι: 'i', Κ: 'K', κ: 'k', Μ: 'M',
  Ν: 'N', ν: 'v', Ο: 'O', ο: 'o', Ρ: 'P', ρ: 'p', Τ: 'T', τ: 't', Υ: 'Y', υ: 'u', Χ: 'X', χ: 'x', Ϲ: 'C', ϲ: 'c',
  ı: 'i', Ł: 'L', ł: 'l', Ø: 'O', ø: 'o', Đ: 'D', đ: 'd', ŋ: 'n', ɡ: 'g', '¡': 'i', '€': 'e',
};
const FROM = '01!|l34@5$7+698', TO = 'oiiiieaassttggb';

/** ch as a letter (above), else '' */
function letter(ch) {
  if (ch >= 'A' && ch <= 'Z') ch = ch.toLowerCase();
  const i = FROM.indexOf(ch);
  if (i >= 0) return TO[i];
  return ch >= 'a' && ch <= 'z' ? ch : '';
}
const word = w => [...w].map(letter).join('');
const PHRASES = HATE.verbs.flatMap(v => ['', ...HATE.between].flatMap(b => HATE.groups.map(g => v + b + g)));
const ANYWHERE = [...SLURS.anywhere, ...PHRASES].map(word), WHOLE = SLURS.whole.map(word), EDGE = SLURS.edge.map(word);
const OK_START = EDGE_OK.start.map(word), OK_END = EDGE_OK.end.map(word), STEMS = PID_STEMS.map(word);
const OK_WHOLE = WHOLE_OK.map(word);
const DYKE = [word('dyke')], NIGER = word('niger'), NIGERIA = word('nigeri');
const digit = ch => ch !== undefined && ch >= '0' && ch <= '9';
/** the run of n digits at str[i] is a number, not letters: three or more, or two with no letter after them or a
 *  capital after a small letter before them */
function number(str, i, n) {
  if (n !== 2) return n > 2;
  const a = str[i + 2] ?? '', b = str[i - 1] ?? '';
  return !a || !letter(a) || (a >= 'A' && a <= 'Z' && b >= 'a' && b <= 'z');
}
/** the end of word w (its letters) at s[i], each letter at least as many times in a row as in w; -1 if it is not
 *  there */
function at(s, i, w) {
  for (let j = 0; j < w.length;) {
    const c = w[j];
    let m = 0, k = 0;
    for (; w[j] === c; j++) m++;
    while (s[i + k] === c) k++;
    if (k < m) return -1;
    i += k;
  }
  return i;
}
/** a "pid" word of the anywhere list (pidor, pidar, pidr) at s[i] is the end of an English word: not at the start of
 *  a part, and right after the start of one of those words ("RapidRacer", "Stupidrift"), or after a vowel and into
 *  the next part ("TopiDrift"); "vladpidr", "dapidor", "YaPidor" are slurs.  starts[j]: a part starts at s[j] */
function englishPid(s, i, w, starts) {
  if (!w.startsWith('pid') || starts[i]) return false;
  if (STEMS.some(st => st.length <= i && s.startsWith(st, i - st.length))) return true;
  if (!'aeiouy'.includes(s[i - 1])) return false;
  for (let j = i + 1, e = at(s, i, w); j < e; j++) if (starts[j]) return true;
  return false;
}
/** a word of the anywhere list at s[i] is no slur there: an English "pid" word, "niger" in "Nigeria" */
const excused = (s, i, w, starts) => englishPid(s, i, w, starts) || (w === NIGER && s.startsWith(NIGERIA, i));
/** t is all one of words, or its plural */
function whole(t, words) {
  return words.some(w => {
    let e = at(t, 0, w);
    if (e < 0) return false;
    while (t[e] === 's') e++;
    return e === t.length;
  });
}

/** t is a word of the whole list (or its plural), and no name of WHOLE_OK */
const wholeWord = t => whole(t, WHOLE) && !OK_WHOLE.includes(t);

/** t starts with word w (its letters, as at() reads them) */
const startsWith = (t, w) => at(t, 0, w) >= 0;
/** t ends with word w, or its plural */
function endsWith(t, w) {
  for (let i = 0; i < t.length; i++) {
    let e = at(t, i, w);
    if (e < 0) continue;
    while (t[e] === 's') e++;
    if (e === t.length) return true;
  }
  return false;
}
/** t starts or ends with a word of the edge list, and not with a word of EDGE_OK there */
const edge = t => EDGE.some(w => (startsWith(t, w) && !OK_START.some(x => startsWith(t, x))) ||
  (endsWith(t, w) && !OK_END.some(x => endsWith(t, x))));

/** name reads as a slur (above) */
export function blockedName(name) {
  const str = [...String(name ?? '').normalize('NFKD').replace(/\p{M}/gu, '')].map(ch => LOOKALIKE[ch] ?? ch);
  // 1488, spaces, _ or - between its digits or not ("14_88"; not "1:48.8", a lap time)
  if (str.filter(ch => !' _-'.includes(ch)).join('').includes('1488')) return true;
  let s = '', t = '', prev = '', low = false;
  const starts = [];
  for (let i = 0; i <= str.length; i++) {
    const ch = str[i] ?? '';
    let r = 0;
    while (digit(str[i + r])) r++;
    let num = r > 0 && number(str, i, r);
    // after a lone n (or ni) and before a letter, digits are letters: "N1663r"
    if (num && (t === 'n' || t === 'ni') && str[i + r] && letter(str[i + r])) num = false;
    const c = num ? '' : ch && letter(ch);
    if ((!c || (low && ch >= 'A' && ch <= 'Z')) && t) {
      if ((wholeWord(t) && !(prev === 'van' && whole(t, DYKE))) || edge(t)) return true;
      prev = t; t = '';
    }
    if (c) {
      if (s.length < 255) { starts[s.length] = !t; s += c; }
      if (t.length < 255) t += c;
    }
    if (!c) low = false;
    else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) low = ch >= 'a';
    if (num) i += r - 1;
  }
  return ANYWHERE.some(w => {
    for (let i = 0; i < s.length; i++) if (at(s, i, w) >= 0 && !excused(s, i, w, starts)) return true;
    return false;
  }) || wholeWord(s) || edge(s);
}

const P_EDGE = PLATE.edge.map(word), P_END_OK = PLATE.endOk.map(word), P_RESTS = PLATE.rests.map(word);
const P_ANY = PLATE.anywhere.map(word), P_ANY_OK = PLATE.anywhereOk.map(word);
/** word x holds s[i..e): it is there from s[j], j <= i, to e or past it */
function held(s, i, e, x) {
  for (let j = Math.max(0, i - x.length); j <= i; j++) if (at(s, j, x) >= e) return true;
  return false;
}
/** plate (a license plate: capitals and digits, at most 8; anything else is read as a name only) reads as a slur, as
 *  a name or as a plate (PLATE) */
export function blockedPlate(plate) {
  const p = String(plate ?? '');
  if (blockedName(p)) return true;
  if (!/^[A-Z0-9]{0,8}$/.test(p)) return false;
  const parts = p.match(/[A-Z]+|[0-9]+/g) ?? [];
  if (parts.some((x, i) => PLATE.alone.includes(x) ||
    (PLATE.numbers.includes(x) && [parts[i - 1] ?? '', parts[i + 1] ?? ''].some(y => PLATE.marks.includes(y))))) return true;
  // its letters as a name's, a lone digit at its end left out
  const q = /[A-Z][0-9]$/.test(p) ? p.slice(0, -1) : p;
  let s = '';
  for (let i = 0; i < q.length; i++) {
    let r = 0;
    while (digit(q[i + r])) r++;
    if (r && number(q, i, r)) i += r - 1;
    else s += letter(q[i]);
  }
  return P_EDGE.some(w => {
    if (endsWith(s, w) && !P_END_OK.some(x => endsWith(s, x))) return true;
    const e = at(s, 0, w);
    return e >= 0 && P_RESTS.some(r => whole(s.slice(e), [r]) || whole(s.slice(e).replace(/^s+/, ''), [r]));
  }) || P_ANY.some(w => {
    for (let i = 0; i < s.length; i++) {
      const e = at(s, i, w);
      if (e >= 0 && !P_ANY_OK.some(x => held(s, i, e, x))) return true;
    }
    return false;
  });
}
