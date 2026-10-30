// Differential check of the settings.ini port: runtime/bundle.m's own C code
// (its key table, validation and file reader, cut out of bundle.m unchanged
// and compiled for this Mac) against web/src/app/settings.ts, on the same
// inputs.  Prints every disagreement; exit 1 if there is one.
//   node web/tools/settings-diff.mjs [--game ug2|mw]
// --game: the page of that game (its template, src/games/<id>/template.ts, behind '@game'; Underground 2's without
// it): the key table and its validation are both games' (bundle.m's), the template's defaults are checked against
// bundle.m's, and every key the template lists has a reader in that game's build: its runtime (runtime/core.list, the
// game's own files: Underground 2's in runtime/, another game's runtime/<id>/, and runtime/web) or the page.
// Builds into out/web/settings-diff/ (the C harness and the bundled module).
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { gameOf } from './rig-game.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(ROOT, 'out/web/settings-diff');
const GAME = gameOf(process.argv.slice(2));
fs.mkdirSync(OUT, { recursive: true });

// ---- the C side: bundle.m lines from the K_* enum to the end of app_load_settings
const src = fs.readFileSync(path.join(ROOT, 'runtime/bundle.m'), 'utf8').split('\n');
const a = src.findIndex(l => l.startsWith('enum { K_RAW'));
const b = src.findIndex(l => l.startsWith('static void settings_put('));
if (a < 0 || b < 0) throw new Error('bundle.m layout changed');
const harness = `#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include "names.h"
static char g_settings[1100];
${src.slice(a, b).join('\n')}
/* stdin: "L <key>\\t<value>" applies one setting (bundle.m's caller trims
 * both), "F <path>" loads a whole settings.ini; then prints every variable
 * of the table and the notes.  Each case starts from a clean environment. */
int main(void){
    char line[4096];
    while (fgets(line, sizeof line, stdin)){
        line[strcspn(line, "\\n")] = 0;
        for (int k = 0; k < NKEYS; k++) unsetenv(g_keys[k].env);
        unsetenv("NFSU2_FOV"); unsetenv("NFSU2_STATUS_EVERY"); unsetenv("NFSU2_MIXED_CASE"); unsetenv("NFSU2_FPS_COUNTER");
        unsetenv("NFSU2_AUDIO_LATENCY"); unsetenv("SOME_VAR"); unsetenv("NFSU2_PAD_LOG"); unsetenv("NFSU2_NETWOR\u212A");
        g_set_logn = 0; g_set_log[0] = 0;
        if (line[0] == 'L'){ char *tab = strchr(line + 2, '\\t'); if (!tab) continue; *tab = 0; apply_setting(line + 2, tab + 1, 1); }
        else if (line[0] == 'F'){ snprintf(g_settings, sizeof g_settings, "%s", line + 2); app_load_settings(); }
        const char *extra[] = { "NFSU2_FOV", "NFSU2_STATUS_EVERY", "NFSU2_MIXED_CASE", "NFSU2_FPS_COUNTER", "NFSU2_AUDIO_LATENCY", "SOME_VAR", "NFSU2_PAD_LOG",
                                "NFSU2_NETWOR\u212A" };
        printf("{");
        int first = 1;
        for (int k = 0; k < NKEYS + 8; k++){
            const char *e = k < NKEYS ? g_keys[k].env : extra[k - NKEYS];
            const char *v = getenv(e);
            if (v){ printf("%s\\"%s\\":\\"", first ? "" : ",", e); for (const char *p = v; *p; p++){ if ((unsigned char)*p < 0x20){ printf("\\\\u%04x", *p); continue; } if (*p == '"' || *p == '\\\\') putchar('\\\\'); putchar(*p); } printf("\\""); first = 0; }
        }
        int ign = 0; for (const char *p = g_set_log; (p = strstr(p, "ignored")); p++) ign++;
        printf("%s\\"#ignored\\":\\"%d\\"}\\n", first ? "" : ",", ign);
        fflush(stdout);
    }
    return 0;
}
`;
fs.writeFileSync(path.join(OUT, 'harness.c'), harness);
execFileSync('clang', ['-O1', '-w', '-I', path.join(ROOT, 'runtime'), '-o', path.join(OUT, 'harness'), path.join(OUT, 'harness.c')]);

// ---- the TypeScript side
await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/settings.ts')], bundle: true, format: 'esm', platform: 'node',
  outfile: path.join(OUT, `settings-${GAME}.mjs`), logLevel: 'silent', alias: { '@game': path.join(ROOT, 'web/src/games', GAME) } });
const S = await import(path.join(OUT, `settings-${GAME}.mjs`) + `?t=${Date.now()}`);

// ---- cases: every key of the C table with a spread of values
const values = ['on', 'off', 'yes', 'No', 'TRUE', 'false', '1', '0', 'enabled', 'Disabled', 'maybe', '', 'auto', 'Auto', 'native',
  'vanilla', 'MAX', 'uncapped', '60', '120', '240', '144', 'scale:0.5', 'SCALE:1.5', 'scale:abc', 'scale:', '0.5', '1e3', '+5', '-1', '0x10',
  'inf', 'nan', '1.', '.5', '5x', '1024', '65535', '65536', '99', '2', '8', '9', '1000', '1001', '64000', '20000', 'Tester', 'Tester ; comment',
  'Team#1', 'Team #1', 'a;b', '; only comment', 'x # y', '1.2.3.4', '1.2.3.4:80', '1.2.3.4.5', '1.2.3.4:', '256.1.1.1', '1.2.3.4:0',
  '1.2.3.4:65536', 'host.name', '1.2.3.4, 5.6.7.8:9', '1.2.3.4;5.6.7.8', ',', '1.2.3', 'x'.repeat(24), 'y'.repeat(25), 'ünï',
  // crews (K_CREWS): names joined by commas, each at most 24 characters without the blanks round it
  'Blue Room, Night Owls', ` ${'é'.repeat(24)} , a`, `a,${'é'.repeat(25)}`, `${'x'.repeat(24)},${'y'.repeat(24)},z,w,v`, 'a,,b',
  '12345678901234567890123456789012', '1234567890123456789012345678901', '12ab', 'has space', 'https://x.example/y.png', 'asset_key',
  '43', '4:3', 'wide', 'ultrawide', 'hotplug', 'pause', 'run', 'fit', 'fill', 'stretch', 'full', 'minimal', 'backing', 'panel',
  'default,-audio', '2560x1600', '9000000', '0.08', '0.97', '/Users/x/NFSU2', 'skippable', 'Original', 'SKIPPABLE ', 'skip',
  // the kinds bundle.m validates since v12na-port (resolution, hfr_fixes, lan_*, texture_*) and ASCII-only case folding
  '320x320', '319x320', '16384X16384', '16385x400', ' 2560x 1600', '+2560x+1600', '2560x1600 ', '2560 x1600', 'x1600', '2560x',
  'ticks,timer', 'DEFAULT, -Audio', '-', 'ticks,,trail', 'ticks;trail', 'tic\u212As', 'bac\u212Aing', 'world cars\thud', 'All',
  'en0', 'EN0', 'en-0', 'abcdefghijklmnop', '10.0.0.2', '010.000.000.002', '4294967296.1.1.1', '4294967551.1.1.1',
  '99999999999999999999999.1.1.1', '1.2.3.4.', '1..2.3', '02:4e:56:4c:00:02', '2:4E:56:4c:0:2', '00:00:00:00:00:00',
  '02:4e:56:4c:00:02:00', '024:e5:64:c0:00:2', '0x2:4e:56:4c:00:02', 'my-pc', 'MY_PC', 'z'.repeat(63), 'z'.repeat(64),
  'tab\there', 'del\x7f', 'ü'.repeat(31) + 'x', 'ü'.repeat(32), '/tmp/dump', 'Yes', 'enabled ',
  // names that read as a slur (names.h, names.mjs) and others with their letters
  'N1gg 4', 'f4g_42', 'XxCoonXx', 'Mustafa GS', 'Raccoon'];
const keys = [...src.slice(a, b).join('\n').matchAll(/\{ "([a-z_]+)",\s+"([A-Z_0-9]+)"/g)].map(m => m[1]);
const generic = ['NFSU2_STATUS_EVERY', 'fov', 'MiXed_Case', 'SOME_VAR', 'networ\u212A'];
const cases = [];
for (const k of [...keys, ...generic]) for (const v of values) cases.push({ k, v });

const io = cases.map(c => `L ${c.k}\t${c.v}`).join('\n') + '\n';
const cOut = execFileSync(path.join(OUT, 'harness'), { input: io, encoding: 'utf8' }).trim().split('\n').map(l => JSON.parse(l));

let diffs = 0, agree = 0;
const tsEnv = (k, v) => {
  const r = S.checkSetting(k, v);
  if (r === null) return { env: {}, ignored: 0 };
  if (!r.ok) return { env: {}, ignored: 1 };
  return { env: { [r.env]: r.value }, ignored: 0 };
};
cases.forEach((c, i) => {
  const co = cOut[i], ign = +co['#ignored']; delete co['#ignored'];
  const t = tsEnv(c.k, c.v);
  const same = JSON.stringify(co) === JSON.stringify(t.env) && ign === t.ignored;
  if (same) agree++; else { diffs++; if (diffs <= 40) console.log(`DIFF ${c.k} = ${JSON.stringify(c.v)}: C ${JSON.stringify(co)} ignored=${ign}  TS ${JSON.stringify(t.env)} ignored=${t.ignored}`); }
});
console.log(`single settings: ${agree} agree, ${diffs} differ (${cases.length} cases over ${keys.length} table keys + ${generic.length} generic)`);

// ---- whole files through the readers (comments, sections, quotes, duplicates, missing '=')
const files = [
  '# NFSU2 settings, read at every launch.\nmovies = off\n  fps = 120  \n[section]\n; comment\nhud = "43"\nhud = wide\nno equals here\nroam_name = Thom ; me\nroam_room = Team#1\nresolution =\nNFSU2_STATUS_EVERY = 600\npad_log = 1\nrender = scale:0.75\nwatchdog = soon\n',
  'movies=OFF\r\nsound = 0\r\nbackground=Run\r\ngamepad = TRUE\r\n',
  "fps = 'auto'\nmax_pixels = 6000000\nroam_room = Team ; ours\nroam_rate = 20 # default\nroam = yes ; on\n",
];
let fileDiffs = 0;
files.forEach((text, i) => {
  const f = path.join(OUT, `case${i}.ini`); fs.writeFileSync(f, text);
  const co = JSON.parse(execFileSync(path.join(OUT, 'harness'), { input: `F ${f}\n`, encoding: 'utf8' }).trim());
  const ign = +co['#ignored']; delete co['#ignored'];
  const r = S.resolveEnv(text, {}, {});
  const te = Object.fromEntries(Object.entries(r.env).filter(([k]) => r.source[k] === 'settings.ini'));
  const tIgn = r.notes.filter(n => n.includes('ignored')).length;   // as the harness counts bundle.m's notes
  const sortObj = o => JSON.stringify(Object.fromEntries(Object.entries(o).sort()));
  const same = sortObj(co) === sortObj(te) && ign === tIgn;
  if (!same) { fileDiffs++; console.log(`FILE DIFF case${i}: C ${sortObj(co)} ignored=${ign}\n                TS ${sortObj(te)} ignored=${tIgn}\n${r.notes.join('\n')}`); }
  else console.log(`file case${i}: same environment (${Object.keys(te).length} variables, ${ign} lines ignored)`);
});

// ---- the TS-only pieces: template defaults, settings_put, the upgrade
const checks = [];
const allDefaultsOk = [...S.DEFAULTS].every(([k, v]) => v === '' || S.checkSetting(k, v)?.ok);
checks.push(['every template default is accepted', allDefaultsOk]);
// the defaults both templates list ("#key = value"): the same (the settings panel shows the page's), but
// fullscreen (the Mac's empty one remembers the last choice; the page's is off)
const tpl = src.slice(src.findIndex(l => l.startsWith('static const char *const k_template =')) + 1);
const cDefaults = new Map();
for (const l of tpl.slice(0, tpl.findIndex(l => l.trimEnd().endsWith(';')) + 1)) {
  const m = /^"#([a-z_0-9]+) =\s*(.*?)\\n";?$/.exec(l.trim());
  if (m) cDefaults.set(m[1], m[2].trim());
}
const defaultDiffs = [...S.DEFAULTS].filter(([k, v]) => k !== 'fullscreen' && cDefaults.has(k) && cDefaults.get(k) !== v);
for (const [k, v] of defaultDiffs) console.log(`DEFAULT DIFF ${k}: bundle.m "${cDefaults.get(k)}", settings.ts "${v}"`);
checks.push([`the templates' defaults agree (${cDefaults.size} in bundle.m)`, cDefaults.size > 20 && !defaultDiffs.length]);
const uncommented = S.TEMPLATE.split('\n').map(l => /^#[a-z_]+ =/.test(l) ? l.slice(1) : l).join('\n');
const ru = S.resolveEnv(uncommented, {}, {});
checks.push(['the template with every option uncommented gives no ignored line', !ru.notes.some(n => / ignored$/.test(n))]);
const put1 = S.settingsPut(S.TEMPLATE, 'fps', '120');
checks.push(['settingsPut replaces the commented line in place', put1.split('\n').length === S.TEMPLATE.split('\n').length && /^fps = 120$/m.test(put1) && !/^#fps =/m.test(put1)]);
checks.push(['settingsPut(null) restores the template line', S.settingsPut(put1, 'fps', null) === S.TEMPLATE]);
checks.push(['settingsPut appends an unknown key', /\nNFSU2_FOV = renderer\n$/.test(S.settingsPut(S.TEMPLATE, 'NFSU2_FOV', 'renderer'))]);
const old = S.TEMPLATE.split(/\n\n# ---- Sound/)[0] + '\nmovies = off\n';      // an "older" file: no sound/controller/roam paragraphs
const up = S.settingsUpgrade(old);
checks.push(['settingsUpgrade appends the missing paragraphs once', !!up && up.added.includes('audio_latency') && up.added.includes('gamepad') &&
  (!S.DEFAULTS.has('roam') || up.added.includes('roam')) && /^movies = off$/m.test(up.text) && S.settingsUpgrade(up.text) === null]);
checks.push(['settingsUpgrade leaves a file that is not ours alone', S.settingsUpgrade('movies = off\n') === null]);
const env = S.resolveEnv('movies = off\nNFSU2_SAVEDIR = /elsewhere\nHOME = /x\ndiscord = on\n', { NOMOVIES: '0' }, { HOME: '/save', NFSU2_SAVEDIR: '/save' });
checks.push(['environment > settings.ini > default; mounts fixed; Mac-only keys ignored', env.env.NOMOVIES === '0' && env.source.NOMOVIES === 'environment' &&
  env.env.NFSU2_SAVEDIR === '/save' && env.env.HOME === '/save' && !('NFSU2_DISCORD' in env.env && env.source.NFSU2_DISCORD === 'settings.ini') &&
  env.env.SNDSYNC === '0' && env.source.SNDSYNC === 'default']);

// ---- the game's readers: every key its template lists (the browser's, scope 'web') is read by its build: its
// runtime (runtime/core.list's files, the game's own: Underground 2's RT_UG2 in web/build.sh, another game's
// runtime/<id>/*.c, every runtime/web/*.c and runtime/*.h) or the page (web/src but settings.ts, settings-ui.ts and the
// games' folders), by the variable's name as a string, or as a field of the page's environment
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');
const cIn = d => fs.readdirSync(path.join(ROOT, d)).filter(f => /\.[ch]$/.test(f)).map(f => `${d}/${f}`);
const core = read('runtime/core.list').split('\n').map(l => l.replace(/#.*/, '').trim()).filter(l => l.endsWith('.c')).map(f => `runtime/${f}`);
const ug2Files = (/^RT_UG2="([^"]*)"/m.exec(read('web/build.sh'))?.[1] ?? '').split(/\s+/).filter(Boolean).map(f => `runtime/${f}.c`);
const runtimeFiles = [...core, ...(GAME === 'ug2' ? ug2Files : cIn(`runtime/${GAME}`)), ...cIn('runtime/web'), ...cIn('runtime').filter(f => f.endsWith('.h'))];
const tsIn = d => fs.readdirSync(path.join(ROOT, d), { withFileTypes: true }).flatMap(e => e.isDirectory()
  ? (['test', 'games'].includes(e.name) ? [] : tsIn(`${d}/${e.name}`)) : e.name.endsWith('.ts') && !/^settings(-ui)?\.ts$/.test(e.name) ? [`${d}/${e.name}`] : []);
const sources = new Map([...runtimeFiles, ...tsIn('web/src')].filter(f => fs.existsSync(path.join(ROOT, f))).map(f => [f, read(f)]));
const readers = env => [...sources].filter(([, s]) => s.includes(`"${env}"`) || s.includes(`'${env}'`) || s.includes(`.${env}`)).map(([f]) => f);
const unread = [];
for (const k of [...S.DEFAULTS.keys()]) {
  const d = S.keyDef(k);
  if (d.scope !== 'web') continue;
  const r = readers(d.env);
  if (!r.length) unread.push(`${k} (${d.env})`);
  else if (process.argv.includes('--readers')) console.log(`  ${k} (${d.env}): ${r.map(f => path.basename(f)).join(', ')}`);
}
for (const k of unread) console.log(`NO READER ${k}: in neither ${GAME === 'ug2' ? 'Underground 2' : GAME}'s runtime nor the page`);
checks.push([`every key of the ${GAME} template has a reader in its build (${S.DEFAULTS.size} keys, ${runtimeFiles.length} runtime files)`, !unread.length]);
for (const [name, ok] of checks) console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}`);
const bad = diffs + fileDiffs + checks.filter(c => !c[1]).length;
console.log(bad ? `${bad} problem(s)` : 'settings.ts agrees with bundle.m');
process.exit(bad ? 1 : 0);
