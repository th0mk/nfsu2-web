// Names that read as a slur: web/src/app/names.mjs (the page's settings) and runtime/names.h (the game: free roam,
// the Mac's settings), the same lists and the same answers.  The server's port (web/server-rs/crates/names) is
// checked against names.mjs in its crate.
//   node --test web/test/names.test.mjs        (the C side needs clang: Xcode's command line tools)
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { SLURS, EDGE_OK, WHOLE_OK, PID_STEMS, HATE, blockedName } from '../src/app/names.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const haveClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status === 0;

// the ways round the lists: digits and signs for letters, gaps, repeats, case, plurals, other scripts
const BLOCKED = ['nigger', 'N1gg3r', 'n i g g e r', 'n.i.g.g.a', 'NiQQa', 'nlgga', 'n!gga', 'niiiigggaaa', 'xXn1gg4Xx',
  'FAG', 'f4g', 'F A G', 'f4g_42', 'XxFagXx', 'fags', 'Faaag', 'faggot', 'f@gg0t', 'fa66ot', 'tr4nny', 'Trannie', 'r3tard',
  'ibne', 'iBnE_34', 'wetback', 'beaner', 'Ch1nk', 'g00k', 'sp1cs', 'paki', 'k1ke', 'dyke', 'c00n', 'XxCoonXx',
  'towelhead', 'sandnigger', 'junglebunny', 'ni99a', 'NI99A', 'nigga99', 'Fag42', 'Big1Fag', 'Mr5Coon', 'trannies',
  'Dykes', 'Van_Fag',
  // the other offensive names (the live server's, 2026-10-26), 1488 in a name
  'Hitler', 'Hitler3858', 'adolfhitler', 'AdolfH', 'H1tl3r', 'gitler', 'Himmler', 'zigHael', 'NAZZI', 'JewsHunter',
  'nigahunter', 'Niga', 'NigaBoi', 'Nig4', 'MEGANIGA', 'KneeGro', 'CzarnuchJebany', 'CHURKA', 'xoxol', 'pidor',
  'PIDARAZ', 'vladpidr', 'cwel666', 'gigacwel', 'Mongoloid', 'TALIBAN', 'Et_Binladen', 'samrapist', 'RapeAddict',
  'ASphaltMolester', '1488', 'Sam1488', 'kkk1488', 'Sam14881488',
  // the words of the live server's names of 2026-11-02, made up the same ways: niger, nig, a niga or nyga starting
  // or ending a part, digits after a lone n, a pid word after a vowel, Nazi nicknames, hate aimed at Jews
  'Nate_Higgers', 'NateHiggers', 'N8_Higgers', 'Niger', 'Nigerz', 'xN1gerx', 'NIG_ER', 'BigNig', 'nigs', 'nigabro',
  'tinyniga', 'SuperNiga', 'HOLYNIGA', 'gigaNyga', 'MrN1663r', 'BigN199a', 'N166ER', 'nupidor', 'JaPidor', 'LolP1dr',
  'TheAustrianArtist', 'austrian_painter', 'PintorAustriaco', 'Uncle_Adolf', 'Kitler88', 'AdolfFitler', 'XitIer',
  'Kristal_Nacht', 'HateJews', 'JewSlayer', 'GasJews', 'JEBAC_ZYDOW', 'pedophile', 'Pedofilo', 'OsamaBin',
  'Al_Qaeda', 'xChigga', 'Nazista', 'ZyklonB',
  // hate aimed at a group as a phrase, with a word between (2026-11-05)
  'GasTheJews', 'Gas_The_Jews', 'KillAllTheJews', 'HuntTheJews', 'DeathToJews', 'g4sTh3J3ws', 'BurnDaJews',
  // the live server's names of 2026-11-05: spellings round the lists' words, phrases at other groups
  'NatFigger', 'Nat_Figgers', 'BBHITLA', 'Adolph_Hitlr', 'HEIL_HTLER', 'gaskikes67', 'HateTheColored', 'Ihateneegr_s',
  'GoodestGoy', 'TheGoodGoyim', 'PedoDave', 'killnig', 'nibb3r',
  // the live server's names of 2026-11-07, made up the same ways: Nazi words spelled round the lists, nazi starting or
  // ending a part, 1488 with signs between its digits
  'SiegHail', 'Seig_Heil', 'ZigHeil', 'Heil_H', 'HeilAH42', '55Heil', 'Heil_SS', 'Hilter420', 'H1tl8r_x', 'Reltih',
  'MeinFurher', 'WaffenSS', 'TheThirdReich', 'GestapoGT', 'Goering_Fan', 'ReadMeinKampf', 'xXnaziXx', 'supernazi',
  'nazidrift', 'Max14_88', 'Dan_1_4_8_8', 'Jewhater', 'NoJewsAllowed', 'Austrian_Drawer', 'KKK_Klansman',
  'Heil4thReich', 'HolocaustDenial', 'Goebbelz_Fan', 'Big_Hitlor', 'AdolfWasRight', 'Adolf_The_Painter', 'White_Pride_TR',
  'Jew_Kil', 'JewsKiller', 'YahudiSiker', 'Ya_Natsist', 'Nacista', 'thenigaman', 'Nigakill3r', 'FuckZyd',
  'Adolf', 'MrAdolf', 'Adolf_42', 'Ad0lf', 'Adolph', 'Reich', 'R3ICH_TR', 'Holocaust_TR', 'SwastikaKing', 'Mr_Bitler'];
const UNICODE = ['ñigga', 'nіgga', 'ｎｉｇｇａ', 'ΝΙGGΑ', 'n̲i̲g̲g̲a̲', 'ⓕⓐⓖ', 'fаg', 'Ηitler', 'Ｈｉｔｌｅｒ', 'Lander１４８８'];
// other words with those letters in them, the player names this server sees most (Turkish) among them
const ALLOWED = ['Mustafa69', 'MustafaGS', 'Mustafa_GS', 'Mustafa Gamer', 'mustafagamer', 'Rafa96', 'Vefa', 'Safa',
  'Pakize', 'Nigar', 'Nigeria', 'Nigerian', 'Nigel', 'Raccoon', 'Tycoon', 'Cocoon', 'spicy', 'Spice',
  'despicable', 'Pakistan', 'Fagundes', 'Bigger', 'Trigger', 'Digger', 'Knight', 'Chinook', 'Scunthorpe', 'Cooper',
  'Ahmet35', 'Emre_34', 'Gökhan', 'İbrahim', 'Tobi', 'Player', 'NAME', 'OLHOST', 'Speedy', '',
  // a name and a number: a year, two digits, the digits the Online mode adds to a taken name (lobby.rs free_variant)
  'Hakan1995', 'Furkan1999', 'Ozan_1996', 'Can 1969', 'Kaan1998', 'Dani99', 'Toni69', 'Dani99GS', 'Ozan1996GS',
  'Hakan1993TR', 'Dani9912', 'Strannik', 'VanDyke', 'Van Dyke',
  // the live names with the new words' letters in them, other numbers with 88
  'Spider', 'MR.SPIDER', 'Shinigami', 'Zuniga', 'Vlado', 'Vitaliba_Nox', 'Grapes', 'Trapezio', 'Nazim',
  'Nazif', 'Nygaard', 'Mongol', 'Churkin', 'jacob88', 'Jane8848', 'Ahmet1448',
  // English words that end in "pid" before an r or a
  'RapidRacer', 'rapid racer', 'CupidRacer', 'Stupidrift', 'StupidArmy', 'TepidRide',
  // the words that start or end a part as niga or nyga would, the new words' letters, a lone n and a year
  'Hoonigan', 'Hooniga', 'Minigame', 'Minigas', 'Kniga', 'Kunigas', 'Niigaki', 'NIGAMI', 'Nygard', 'TonyGamer',
  'Antony Gaming', 'Brony_Gamer', 'Higgins', 'Higgs', 'Austria', 'AustrianGP', 'Painter', 'Artist', 'Osama',
  'TopiDrift', 'N1995', 'Doran1994TR',
  // next to the words of 2026-11-05
  'Figgy', 'Figgis', 'Fighter', 'Tauhitianboy', 'Tahitien', 'Kshitij', 'Hilti_Passat', 'hellhound057', 'Speedo', 'Torpedo',
  'Goyo204', 'Nigro', 'KonigRider39', 'Rapper', 'Draped', 'KillNight',
  // a hate phrase's words on their own
  'Jewel', 'JewishRacer', 'Andrew', 'Matthew', 'KillerQueen', 'GasMonkey', 'Deadpool', 'Bombshell', 'Shooter',
  // next to the words of 2026-11-07: the names that start (or are) nazi as read, l for i
  'Nazli', 'NAZLI', 'Nazli_GS', 'Nazilli', 'Nazlican', 'Nazir', 'Nazia', 'Nazife', 'Nazik', 'Nazih', 'Nazio',
  'Ignazio', 'Ignazi', 'Schlitter', 'HellsSpawn', 'Hellhammer', 'Sheila', 'Heilig', 'Casinojewel', 'Waffen',
  'Panzer', 'Reichert', 'Car14_89', 'Nazlı', 'Nazib', 'Shitlord', 'Goebel',
  'Kaiserreich', 'Adolfo', 'Adolfinho', 'Swastik', 'Zyklon', 'Rudolf'];

test('names.mjs: the lists\' words, the ways round them, and not the other words', () => {
  for (const w of [...SLURS.anywhere, ...SLURS.whole, ...SLURS.edge]) assert.ok(blockedName(w), w);
  for (const v of HATE.verbs) for (const b of ['', ...HATE.between]) for (const g of HATE.groups) assert.ok(blockedName(v + b + g), v + b + g);
  for (const n of [...BLOCKED, ...UNICODE]) assert.ok(blockedName(n), n);
  for (const n of ALLOWED) assert.ok(!blockedName(n), n);
  // a word of the whole list only as a part on its own
  assert.ok(!blockedName('xfagx') && blockedName('x_fag_x') && blockedName('xFagX'));
});

test('names.h has the lists of names.mjs', () => {
  const h = fs.readFileSync(path.join(REPO, 'runtime/names.h'), 'utf8');
  const list = k => [...new RegExp(`\\b${k}\\[\\] = \\{([^}]*)\\}`).exec(h)[1].matchAll(/"([^"]*)"/g)].map(m => m[1]);
  assert.deepEqual(list('anywhere'), [...SLURS.anywhere]);
  assert.deepEqual(list('whole'), [...SLURS.whole]);
  assert.deepEqual(list('edge'), [...SLURS.edge]);
  assert.deepEqual(list('edge_ok_start'), [...EDGE_OK.start]);
  assert.deepEqual(list('edge_ok_end'), [...EDGE_OK.end]);
  assert.deepEqual(list('whole_ok'), [...WHOLE_OK]);
  assert.deepEqual(list('stems'), [...PID_STEMS]);
  assert.deepEqual(list('hate_verbs'), [...HATE.verbs]);
  assert.deepEqual(list('hate_between'), [...HATE.between]);
  assert.deepEqual(list('hate_groups'), [...HATE.groups]);
});

test('names.h answers as names.mjs (25,000 ASCII names)', { skip: !haveClang && 'no clang' }, () => {
  const out = fs.mkdtempSync(path.join(OUT, 'names-c-'));
  try {
    const src = path.join(out, 'names_check.c'), bin = path.join(out, 'names_check');
    fs.writeFileSync(src, `#include <stdio.h>
#include <string.h>
#include "names.h"
int main(void){
    char line[512];
    while (fgets(line, sizeof line, stdin)){ line[strcspn(line, "\\n")] = 0; putchar('0' + name_blocked(line)); }
    putchar('\\n');
    return 0;
}
`);
    execFileSync('clang', ['-O1', '-Wall', '-Wextra', '-Werror', '-I', path.join(REPO, 'runtime'), '-o', bin, src]);
    // the cases above, then names made of the lists' pieces with the ways round them, and plain noise
    let seed = 7;
    const rnd = n => { seed = (seed * 1103515245 + 12345) >>> 0; return (seed >>> 8) % n; };
    const pick = a => a[rnd(a.length)];
    const pieces = [...SLURS.anywhere, ...SLURS.whole, ...SLURS.edge, ...EDGE_OK.start, ...EDGE_OK.end, ...WHOLE_OK, ...PID_STEMS,
      ...HATE.verbs, ...HATE.between, ...HATE.groups, 'jewel', 'mustafa', 'gs',
      'raccoon', 'spice', 'niger', 'nigeria', 'pakize', 'xx', 'gamer', 'a', 'g', 'dani', 'hakan', 'van', 'n', 'pid', 'topi', 'drift',
      '99', '1995', '1663', '69', '1488', '88', '14', '1_4', '8-8'];
    const swap = { o: '0', i: '1!|l', e: '3', a: '4@', s: '5$', t: '7+', g: '69q', b: '8' };
    const names = [...BLOCKED, ...ALLOWED.filter(n => /^[ -~]*$/.test(n))];
    for (let k = 0; k < 20000; k++) {
      let n = '';
      for (let j = 1 + rnd(3); j > 0; j--) {
        for (const ch of pick(pieces)) {
          const r = rnd(10), alt = swap[ch];
          n += r === 0 && alt ? pick([...alt]) : r === 1 ? ch.toUpperCase() : r === 2 ? ch + ch : ch;
          if (rnd(12) === 0) n += pick([' ', '_', '-', '.', '2']);
        }
        if (rnd(3) === 0) n += pick(['', ' ', '_', '42']);
      }
      names.push(n.slice(0, 24));
    }
    for (let k = 0; k < 5000; k++) { let n = ''; for (let j = rnd(25); j > 0; j--) n += String.fromCharCode(32 + rnd(95)); names.push(n); }
    const c = execFileSync(bin, { input: names.join('\n') + '\n', encoding: 'utf8', timeout: 120_000 }).trim();
    assert.equal(c.length, names.length);
    const js = names.map(n => (blockedName(n) ? '1' : '0')).join('');
    const diff = names.filter((n, i) => c[i] !== js[i]);
    assert.deepEqual(diff, [], 'names the two read differently');
    const blocked = [...js].filter(x => x === '1').length;
    assert.ok(blocked > 1000 && blocked < names.length - 1000, `both answers among the made-up names (${blocked} of ${names.length} blocked)`);
  } finally { fs.rmSync(out, { recursive: true, force: true }); }
});
