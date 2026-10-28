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
import { SLURS, blockedName } from '../src/app/names.mjs';

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
  'ASphaltMolester', '1488', 'Sam1488', 'kkk1488', 'Sam14881488'];
const UNICODE = ['ñigga', 'nіgga', 'ｎｉｇｇａ', 'ΝΙGGΑ', 'n̲i̲g̲g̲a̲', 'ⓕⓐⓖ', 'fаg', 'Ηitler', 'Ｈｉｔｌｅｒ', 'Lander１４８８'];
// other words with those letters in them, the player names this server sees most (Turkish) among them
const ALLOWED = ['Mustafa69', 'MustafaGS', 'Mustafa_GS', 'Mustafa Gamer', 'mustafagamer', 'Rafa96', 'Vefa', 'Safa',
  'Pakize', 'Nigar', 'Niger', 'Nigeria', 'Nigerian', 'Nigel', 'Raccoon', 'Tycoon', 'Cocoon', 'spicy', 'Spice',
  'despicable', 'Pakistan', 'Fagundes', 'Bigger', 'Trigger', 'Digger', 'Knight', 'Chinook', 'Scunthorpe', 'Cooper',
  'Ahmet35', 'Emre_34', 'Gökhan', 'İbrahim', 'Thom', 'Player', 'NAME', 'OLHOST', 'Speedy', '',
  // a name and a number: a year, two digits, the digits the Online mode adds to a taken name (lobby.rs free_variant)
  'Hakan1995', 'Furkan1999', 'Ozan_1996', 'Can 1969', 'Kaan1998', 'Dani99', 'Toni69', 'Dani99GS', 'Ozan1996GS',
  'Hakan1993TR', 'Dani9912', 'Strannik', 'VanDyke', 'Van Dyke',
  // the live names with the new words' letters in them, other numbers with 88
  'Spider', 'MR.SPIDER', 'Shinigami', 'Zuniga', 'Vlado', 'Ataliba_Neto', 'Grapes', 'Trapezio', 'Adolf', 'Nazim',
  'Nazif', 'Nygaard', 'Mongol', 'Churkin', 'jacob88', 'Jane8848', 'Ahmet1448',
  // English words that end in "pid" before an r or a
  'RapidRacer', 'rapid racer', 'CupidRacer', 'Stupidrift', 'StupidArmy', 'TepidRide'];

test('names.mjs: the lists\' words, the ways round them, and not the other words', () => {
  for (const w of [...SLURS.anywhere, ...SLURS.whole]) assert.ok(blockedName(w), w);
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
    const pieces = [...SLURS.anywhere, ...SLURS.whole, 'mustafa', 'gs', 'raccoon', 'spice', 'niger', 'pakize', 'xx', 'gamer', 'a', 'g',
      'dani', 'hakan', 'van', '99', '1995', '69', '1488', '88'];
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
    const c = execFileSync(bin, { input: names.join('\n') + '\n', encoding: 'utf8' }).trim();
    assert.equal(c.length, names.length);
    const js = names.map(n => (blockedName(n) ? '1' : '0')).join('');
    const diff = names.filter((n, i) => c[i] !== js[i]);
    assert.deepEqual(diff, [], 'names the two read differently');
    const blocked = [...js].filter(x => x === '1').length;
    assert.ok(blocked > 1000 && blocked < names.length - 1000, `both answers among the made-up names (${blocked} of ${names.length} blocked)`);
  } finally { fs.rmSync(out, { recursive: true, force: true }); }
});
