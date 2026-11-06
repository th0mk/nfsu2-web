// The Mods tab's catalogue (web/src/app/mods.ts):
//   node --test web/test/mods.test.mjs
// - the entries: ids once each, in the sections' order, a row shared only with an entry of its own section and
//   setting, each with a line and a short popup, links of their own kinds, no texts the repo must not have
// - our changes (ADDED): no rows of their own among the mods' ('ours'), a row per group, each group with changes,
//   each change named once and not as a mod's row is, with a short line
// - the credits: a port's author after the mod's own, and only where the port is the mod's code or files; the source
//   repo's owner the mod's own
// - the controls: each setting a key of settings.ts with a line in its template, the values ones it takes, the
//   places and the keys elsewhere ones the page has; a setting's entry has the file its KEYS comment names, an
//   always-on one none of those files; phones as device.ts's deviceDefaults has them; pending only while its files or
//   its setting are missing
// - the rows: each setting's row in the Mods tab only, once (settings-ui.ts PANEL_KEYS: not under "Other settings in
//   this file" either), with a control of the setting's own kind and values
// - the files: each one there (but a pending entry's), each one mod's, a mod's licence file with its ported code and
//   of the entry's licence
// - the runtime's credits: every runtime/**/*.c, .h and .inc and tools/**/*.py that names a mod anywhere (its
//   nfsmods.xyz page, a GitHub repo, an entry's source repo, mod name or author's name, or a mod's licence) is in the
//   files of that mod's entry (or listed below as naming it without a port); every mod named by a link is an entry's
//   (or listed below as not the tab's); every licences/ file is an entry's.  Still unseen: a port whose files name
//   nothing of the mod, and a mod named only by a name no entry has.
// The modules are bundled from web/src with esbuild, as web/test/flags.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/settings.ts'; export * as M from './web/src/app/mods.ts'; export * as D from './web/src/app/device.ts';
  export * as UI from './web/src/app/settings-ui.ts'; export * as MU from './web/src/app/mods-ui.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const { M, D, UI, MU } = S;
const MODS = M.MODS;

/** files that name a mod but port none of it: the entries whose mod they only name (true: every mod they name) */
const NOT_PORTS = {
  // the patch sites of other mods, which the free-roam plugin for the PC game keeps clear of, and its check
  'tools/roam_modsites.py': true,
  'tools/asi_check.py': true,
  // WidescreenFix's SkipIntro, named for the way it skips a movie
  'runtime/intro.c': ['widescreen'],
  // the Mac's texture pack builder: its upscaling models' and a library's licences
  'tools/texpack/texpack.py': true,
};
/** what the runtime names that is not the tab's */
const NOT_THE_TABS = {
  // the language packs (KyoriAsh's Multi 13 languages and the other translations): the About panel's Translations tab
  // credits them; tools/langpacks.py is in the tab for English Subtitles Fixed, which it also builds
  'nfsmods.xyz/mod/5749': true,
  // oldman777's script, the same change as Show Hidden Races Mod: that entry's popup names it
  'nfsmods.xyz/mod/5048': true,
  // TerminatorVasya's Shadow Fix: a fix of a few lines (runtime/carshadow.c), always on, not a row of its own
  'nfsmods.xyz/mod/7328': true,
  'github.com/tvasya/nfsu2shadowfix': true,
  // a library (runtime/xxhash.h), not a mod of the game
  'github.com/cyan4973/xxhash': true,
  'github.com/google/highwayhash': true,
};
/** other names the runtime gives a mod: the mod's name (as an entry's mod or name has it) */
const ALIASES = { ExtraOptions: 'NFSU2 Extra Options', WidescreenFix: 'Widescreen Fixes Pack' };
/** licences/ files that are not a mod's: the flags' pictures (flags.ts) */
const NOT_MOD_LICENCES = ['licenses/flag-icons.txt'];
/** a licence as a source names it, and its text's first line */
const LICENCES = { MIT: [/\bMIT\b/, /^(The )?MIT License/], 'GPL-3.0': [/\bGPL-3\.0\b/, /^\s*GNU GENERAL PUBLIC LICENSE\s+Version 3/] };

const exists = (p) => fs.existsSync(path.join(ROOT, p));
const texts = (m) => [m.name, m.mod, m.line, m.details, ...m.authors.map(a => a.name), ...(m.parts ?? []).flatMap(p => [p.name, p.text])].filter(Boolean);
/** the mod an entry is (or is a part of) */
const modOf = (m) => m.mod ?? m.name;
const linkOf = (url) => url.replace(/^https:\/\//, '').toLowerCase();

test('the entries: ids once each, in the sections\' order', () => {
  const ids = MODS.map(m => m.id);
  assert.equal(new Set(ids).size, ids.length, 'ids unique');
  for (const id of ids) assert.match(id, /^[a-z0-9-]+$/, id);
  const kinds = M.KINDS.map(k => k.kind);
  assert.deepEqual(kinds, ['screen', 'shared', 'rules', 'car']);
  let at = 0;
  for (const m of MODS) {
    const k = kinds.indexOf(m.kind);
    assert.ok(k >= 0, `${m.id}: kind ${m.kind}`);
    assert.ok(k >= at, `${m.id}: in its section's place (${m.kind} after ${kinds[at]})`);
    at = k;
  }
  for (const k of M.KINDS) assert.ok(k.title, k.kind);
});

test('our changes: a row per group, every change in one, none named as a mod\'s row', () => {
  // none of the mods' rows is ours any more: our changes are ADDED's
  assert.deepEqual(MODS.filter(m => m.kind === 'ours').map(m => m.id), [], 'no \'ours\' rows');
  assert.ok(M.OURS.title, 'OURS');
  assert.deepEqual(M.ADDED.map(g => g.title), ['Driving', 'Free roam', 'Photos and screen', 'Career and garage', 'Controls', 'Saves']);
  const ids = M.ADDED.map(g => g.id), modIds = new Set(MODS.map(m => m.id));
  assert.equal(new Set(ids).size, ids.length, 'group ids unique');
  for (const id of ids) {
    assert.match(id, /^[a-z0-9-]+$/, id);
    assert.ok(!modIds.has(id) && id !== 'ours' && !M.KINDS.some(k => k.kind === id), `${id}: a mod's id or a section's (the (i) finds its row by it)`);
  }
  // the mods' rows' names, their entries' and their parts' (lower case)
  const rows = MODS.filter(m => !m.row).map(m => MU.rowName(m.id));
  const modNames = new Set([...rows, ...MODS.flatMap(m => [m.name, m.mod, ...(m.parts ?? []).map(p => p.name)])].filter(Boolean).map(n => n.toLowerCase()));
  assert.ok(modNames.has('upscaled hud and detailed map'), 'a shared row\'s name');
  const seen = new Set();
  for (const g of M.ADDED) {
    assert.ok(g.items.length > 0, `${g.id}: changes`);
    for (const c of g.items) {
      assert.ok(c.name.trim() && c.line.trim(), `${g.id}: ${c.name}`);
      assert.ok(!seen.has(c.name.toLowerCase()), `${c.name}: in one group, once`);
      seen.add(c.name.toLowerCase());
      assert.ok(!modNames.has(c.name.toLowerCase()), `${c.name}: a Mods row's name`);
      assert.equal(typeof c.key, 'string', `${c.name}: key`);
      assert.equal(c.key, c.key.trim(), `${c.name}: key`);
      assert.ok(c.line.length <= 50, `${c.name}: line of ${c.line.length} characters`);
      assert.doesNotMatch(c.line, /[.]$/, `${c.name}: a line, not a sentence`);
    }
  }
});

test('a shared row: one entry\'s row, the same section and setting', () => {
  const byId = new Map(MODS.map(m => [m.id, m]));
  for (const m of MODS.filter(x => x.row)) {
    const first = byId.get(m.row);
    assert.ok(first, `${m.id}: row ${m.row}`);
    assert.ok(MODS.indexOf(first) < MODS.indexOf(m), `${m.id}: after ${first.id}`);
    assert.equal(first.row, undefined, `${m.id}: ${first.id} has a row of its own`);
    assert.equal(first.kind, m.kind, m.id);
    assert.ok('setting' in m.control && 'setting' in first.control && m.control.setting === first.control.setting, `${m.id}: ${first.id}'s setting`);
  }
  // two entries with one setting share a row
  const bySetting = new Map();
  for (const m of MODS.filter(x => 'setting' in x.control && !x.row)) {
    assert.ok(!bySetting.has(m.control.setting), `${m.id}: ${m.control.setting} has ${bySetting.get(m.control.setting)}'s row: row: '${bySetting.get(m.control.setting)}'`);
    bySetting.set(m.control.setting, m.id);
  }
});

test('the texts: a line, a short popup, no words the repo must not have', () => {
  for (const m of MODS) {
    assert.ok(m.name.trim(), m.id);
    assert.ok(m.line.length > 0 && m.line.length <= 90, `${m.id}: line of ${m.line.length} characters`);
    const sentences = m.details.split(/(?<=[.!?])\s+/).filter(Boolean).length;
    assert.ok(sentences >= 2 && sentences <= 4, `${m.id}: ${sentences} sentences`);
    assert.ok(/[.!?]$/.test(m.details), `${m.id}: details end a sentence`);
    assert.ok(Object.hasOwn(M.ONLINE, m.online), `${m.id}: online ${m.online}`);
    // a mod's parts: a name, what it does in a sentence or two, who sees it
    for (const p of m.parts ?? []) {
      assert.ok(p.name.trim() && /[.!?]$/.test(p.text) && p.text.length <= 240, `${m.id}: part ${p.name}`);
      assert.ok(Object.hasOwn(M.ONLINE, p.online), `${m.id}: part ${p.name}: online ${p.online}`);
    }
    if (m.parts) assert.ok(m.parts.length >= 2 && new Set(m.parts.map(p => p.name)).size === m.parts.length, `${m.id}: parts`);
    // "With it off: as in the original game" but where the entry says otherwise (off: false), a setting's only; the
    // popup's facts say it, its details not again
    if (m.off !== undefined) assert.ok(m.off === false && 'setting' in m.control, `${m.id}: off`);
    if ('setting' in m.control && m.off !== false) assert.doesNotMatch(m.details, /\bOff: as in the original game\b/, `${m.id}: the facts say it`);
  }
  const all = [...MODS.flatMap(m => [...texts(m), ...Object.values(m.links), ...m.authors.map(a => a.url ?? '')]),
    ...M.KINDS.map(k => k.title), M.OURS.title, ...M.ADDED.flatMap(g => [g.title, ...g.items.flatMap(c => [c.name, c.key, c.line])]),
    ...Object.values(M.PLACES), ...Object.values(M.ONLINE), ...Object.values(M.USES), ...Object.values(M.LABELS)];
  for (const s of all) for (const bad of [/permission/i, /http:\/\//i]) assert.doesNotMatch(s, bad);
});

test('the credits: authors, links and licences of their own kinds', () => {
  for (const m of MODS) {
    assert.ok(m.authors.length > 0, `${m.id}: authors`);
    // the mod's own authors first; a port's only where the port is the mod's code or files
    const port = m.authors.findIndex(a => a.port);
    if (port >= 0) {
      assert.ok(m.authors.slice(port).every(a => a.port), `${m.id}: the port's authors after the mod's own`);
      assert.ok(m.uses === 'code' || m.uses === 'data', `${m.id}: a port of findings`);
    }
    // the source repo's owner is the mod's own author
    for (const a of m.authors) if (a.url && m.links.source && linkOf(m.links.source).startsWith(`${linkOf(a.url)}/`))
      assert.ok(!a.port, `${m.id}: ${a.name} owns ${m.links.source}: the mod's author, not a port's`);
    assert.ok(Object.hasOwn(M.USES, m.uses), `${m.id}: uses ${m.uses}`);
    if (m.links.page) assert.match(m.links.page, /^https:\/\/nfsmods\.xyz\/mod\/\d+$/, m.id);
    if (m.links.source) assert.match(m.links.source, /^https:\/\/github\.com\/[\w.-]+\/[\w.-]+$/, m.id);
    for (const a of m.authors) if (a.url) assert.match(a.url, /^https:\/\//, `${m.id}: ${a.name}`);
    // a link, but for a mod made for this port (its source is this repo's)
    assert.ok((m.links.page || m.links.source) ? m.uses !== 'original' : m.uses === 'original', `${m.id}: a link`);
    if (m.licence) assert.ok(Object.hasOwn(LICENCES, m.licence), m.id);
    if (m.sizeMB !== undefined) assert.ok(m.sizeMB > 0 && m.sizeMB < 1000, m.id);
  }
});

/** the runtime files each key's comment in settings.ts KEYS names (the ones that read it) */
function keyReaders() {
  const src = fs.readFileSync(path.join(ROOT, 'web/src/app/settings.ts'), 'utf8');
  const from = src.indexOf('export const KEYS'), block = src.slice(from, src.indexOf('\n];', from));
  const out = new Map();
  let note = '';
  for (const line of block.split('\n')) {
    if (/^\s*\/\//.test(line)) { note += line; continue; }
    const k = line.match(/\{\s*key:\s*'(\w+)'/);
    if (k) out.set(k[1], [...note.matchAll(/runtime\/[\w/]+\.c\b/g)].map(r => r[0]));
    note = '';
  }
  assert.ok(out.size > 50 && out.get('subtitles')?.includes('runtime/language.c'), 'KEYS read');
  return out;
}

/** device.ts's defaults on a phone (an iPhone's Safari) */
function phoneDefaults() {
  const names = ['navigator', 'screen', 'matchMedia', 'localStorage'];
  const saved = names.map(k => [k, Object.getOwnPropertyDescriptor(globalThis, k)]);
  const put = (k, value) => Object.defineProperty(globalThis, k, { value, configurable: true, writable: true });
  put('navigator', { userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 Mobile/15E148', maxTouchPoints: 5 });
  put('screen', { width: 390, height: 844 });
  put('matchMedia', () => ({ matches: true }));
  put('localStorage', { getItem: () => null });
  try {
    assert.equal(D.deviceKind(), 'phone');
    return D.deviceDefaults();
  } finally {
    for (const [k, d] of saved) if (d) Object.defineProperty(globalThis, k, d); else delete globalThis[k];
  }
}

test('the controls: settings.ini keys and values, places of the page and the game', () => {
  const takes = (key, v) => { const c = S.checkSetting(key, v); return c?.ok === true; };
  const readers = keyReaders();
  const phones = phoneDefaults();
  for (const m of MODS) {
    const c = m.control;
    const kinds = ['setting', 'always', 'elsewhere', 'menu'].filter(k => k in c);
    assert.equal(kinds.length, 1, `${m.id}: one kind of control`);
    if (m.pending) {
      // its files and its setting come with it; once they are here, it is no longer pending
      const missing = m.files.filter(f => !exists(f));
      if ('setting' in c && !S.keyDef(c.setting)) missing.push(c.setting);
      assert.ok(missing.length > 0, `${m.id}: its files and its setting are here: no longer pending`);
    }
    if ('setting' in c) {
      const d = S.keyDef(c.setting);
      if (m.pending && !d) continue;
      assert.ok(d, `${m.id}: ${c.setting} in KEYS`);
      assert.equal(d.key, c.setting, `${m.id}: as KEYS spells it`);
      assert.equal(d.scope, 'web', `${m.id}: ${c.setting} works in the browser`);
      assert.ok(S.DEFAULTS.has(c.setting), `${m.id}: ${c.setting} has a line in TEMPLATE`);
      if (d.kind === 'enum') {
        assert.ok(c.values?.length, `${m.id}: the values of ${c.setting} that turn it on`);
        const vals = d.vals.split('|');
        for (const v of c.values) assert.ok(vals.includes(v), `${m.id}: ${c.setting} = ${v} is one of ${d.vals}`);
      } else {
        assert.equal(d.kind, 'bool', `${m.id}: ${c.setting} a switch or a choice`);
        assert.ok(!c.values || c.values.every(v => v === 'on'), `${m.id}: a switch's values`);
      }
      const read = readers.get(c.setting) ?? [];
      if (read.length) assert.ok(read.some(f => m.files.includes(f)), `${m.id}: ${c.setting} is read by ${read.join(' or ')} (KEYS): in its files`);
      assert.equal(m.phones, phones[c.setting], `${m.id}: phones as deviceDefaults has ${c.setting}`);
      if (m.phones !== undefined) assert.ok(takes(c.setting, m.phones), `${m.id}: phones ${m.phones}`);
    } else {
      assert.equal(m.phones, undefined, `${m.id}: phones without a setting`);
      if ('always' in c) {
        assert.equal(c.always, true, m.id);
        for (const [key, files] of readers) for (const f of files)
          assert.ok(!m.files.includes(f), `${m.id}: always on, but ${f} reads ${key} (KEYS): a setting's`);
      }
      if ('elsewhere' in c) {
        assert.ok(Object.hasOwn(M.PLACES, c.elsewhere), `${m.id}: ${c.elsewhere}`);
        assert.ok(c.keys.length > 0, m.id);
        for (const k of c.keys) assert.ok(S.keyDef(k)?.scope === 'web' && S.DEFAULTS.has(k), `${m.id}: ${k} in KEYS and TEMPLATE`);
      }
      if ('menu' in c) assert.ok(Object.hasOwn(M.PLACES, c.menu), `${m.id}: ${c.menu}`);
    }
  }
});

test('the rows: each setting\'s row in the Mods tab only, once, with a control of its kind', () => {
  const settings = [...new Set(MODS.filter(m => 'setting' in m.control).map(m => m.control.setting))];
  assert.ok(settings.includes('visual_rep_remover') && settings.includes('ui_textures'), 'the mods\' settings');
  for (const key of settings) {
    // one row on the page (PANEL_KEYS lists every row's key: a key twice is a row in two places), so not a line under
    // "Other settings in this file" either
    assert.equal(UI.PANEL_KEYS.filter(k => k === key).length, 1, `${key}: one row`);
    const c = UI.MOD_CONTROLS[key], d = S.keyDef(key);
    assert.ok(c, `${key}: its control in settings-ui.ts MOD_CONTROLS`);
    if (d.kind === 'bool') assert.equal(c.t, 'switch', `${key}: a switch`);
    else {
      assert.ok(c.t === 'seg' || c.t === 'select', `${key}: a choice`);
      assert.deepEqual(c.opts.map(([v]) => v).sort(), d.vals.split('|').sort(), `${key}: the values it takes`);
    }
  }
  assert.deepEqual(Object.keys(UI.MOD_CONTROLS).sort(), [...settings].sort(), 'MOD_CONTROLS: the mods\' settings only');
  // the site's own settings keep their rows elsewhere: Music in drag and drift races in the Game tab
  assert.equal(UI.PANEL_KEYS.filter(k => k === 'race_music').length, 1, 'race_music: one row');
  assert.ok(!settings.includes('race_music'), 'race_music: not a mod\'s');
});

test('the files: there, one mod\'s each, and a ported mod\'s licence with it', () => {
  const owners = new Map();
  for (const m of MODS) {
    assert.ok(m.files.length > 0, m.id);
    for (const f of m.files) {
      assert.ok(!path.isAbsolute(f) && !f.includes('..') && !f.includes('\\'), `${m.id}: ${f}`);
      if (!m.pending) assert.ok(exists(f), `${m.id}: ${f}`);
      owners.set(f, [...(owners.get(f) ?? []), m]);
    }
    if (m.uses === 'code') assert.ok(m.files.some(f => f.startsWith('licenses/') && exists(f)), `${m.id}: its licence in licenses/`);
    for (const f of m.files.filter(x => x.startsWith('licenses/') && exists(x))) {
      assert.ok(m.licence, `${m.id}: ${f} without a licence`);
      assert.match(fs.readFileSync(path.join(ROOT, f), 'utf8').trimStart(), LICENCES[m.licence][1], `${m.id}: ${f} is ${m.licence}`);
    }
  }
  // a file in two entries: parts of one mod, or one row's
  const sameMod = (a, b) => modOf(a) === modOf(b) || a.row === b.id || b.row === a.id;
  for (const [f, ms] of owners) for (const m of ms.slice(1))
    assert.ok(sameMod(ms[0], m), `${f}: in ${ms[0].id} and ${m.id}, two mods`);
});

/** the mods a source names: nfsmods.xyz/mod/<n> and github.com/<owner>/<repo> (as links), mods by an entry's source
 *  repo, mod name or author's name (as mod names), and licences */
function named(text, repos, names) {
  // one line, without the comments' line starts (a link may go on in the next line)
  const src = text.replace(/^[ \t]*(?:\/\*+|\*+\/?|\/\/|#)?/gm, ' ').replace(/\s+/g, ' ');
  const links = new Set(), mods = new Set(), licences = new Set();
  for (const r of src.matchAll(/nfsmods\.xyz\/mod\/(\d+)/g)) links.add(`nfsmods.xyz/mod/${r[1]}`);
  for (const r of src.matchAll(/github\.com\/\s*([\w-]+)\/([\w.-]*\w)/g)) links.add(`github.com/${r[1]}/${r[2]}`.toLowerCase());
  const low = src.toLowerCase();
  for (const repo of repos) if (low.includes(repo)) links.add(`github.com/${repo}`);
  for (const [re, mod] of names) if (re.test(src)) for (const x of mod) mods.add(x);
  for (const [l, [re]] of Object.entries(LICENCES)) if (re.test(src)) licences.add(l);
  return { links, mods, licences };
}

test('the runtime\'s credits: every file that names a mod is in its entry', () => {
  const walk = (dir, exts) => fs.readdirSync(path.join(ROOT, dir), { withFileTypes: true })
    .flatMap(e => e.isDirectory() ? walk(`${dir}/${e.name}`, exts) : exts.some(x => e.name.endsWith(x)) ? [`${dir}/${e.name}`] : []);
  const files = [...walk('runtime', ['.c', '.h', '.inc']), ...walk('tools', ['.py'])];
  const theirs = MODS;
  const repos = theirs.map(m => m.links.source).filter(Boolean).map(u => linkOf(u).replace(/^github\.com\//, ''));
  const word = (s) => new RegExp(`(?<![\\w.])${s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(?!\\w)`);
  const byName = new Map();
  const add = (n, mod) => byName.set(n, new Set([...(byName.get(n) ?? []), mod]));
  for (const m of theirs) { add(modOf(m), modOf(m)); for (const a of m.authors) add(a.name, modOf(m)); }
  for (const [n, mod] of Object.entries(ALIASES)) {
    assert.ok(theirs.some(m => modOf(m) === mod), `ALIASES: ${mod} is an entry's`);
    add(n, mod);
  }
  const names = [...byName].map(([n, mods]) => [word(n), mods]);
  const byLink = new Map();
  for (const m of theirs) for (const u of Object.values(m.links)) byLink.set(linkOf(u), [...(byLink.get(linkOf(u)) ?? []), m]);
  let naming = 0;
  for (const f of files) {
    const { links, mods, licences } = named(fs.readFileSync(path.join(ROOT, f), 'utf8'), repos, names);
    if (!links.size && !mods.size && !licences.size) continue;
    naming++;
    const not = NOT_PORTS[f];
    if (not === true) continue;
    const ok = (ms, what) => {
      if (ms.some(m => not?.includes(m.id))) return;
      assert.ok(ms.some(m => m.files.includes(f)), `${f} names ${what}: put it in the files of ${ms.map(m => m.id).join(' or ')} in web/src/app/mods.ts`);
    };
    for (const l of links) {
      if (NOT_THE_TABS[l]) continue;
      assert.ok(byLink.has(l), `${f} names ${l}, which no entry of web/src/app/mods.ts links to`);
      ok(byLink.get(l), l);
    }
    for (const mod of mods) ok(theirs.filter(m => modOf(m) === mod), mod);
    for (const l of licences) {
      const ms = MODS.filter(m => m.licence === l);
      assert.ok(ms.length, `${f} names ${l}, which is no entry's licence`);
      ok(ms, `the ${l} licence`);
      for (const m of MODS.filter(x => x.files.includes(f) && x.licence)) assert.equal(m.licence, l, `${f} names ${l}: ${m.id}'s licence`);
    }
  }
  assert.ok(naming >= 20, `${naming} files name a mod`);
  // the scan finds what it should
  const got = named('/* x (nfsmods.xyz/mod/7328, github.com/\n * TVasya/NFSU2ShadowFix: y), MIT */\nint a; // as ThirteenAG\'s WidescreenFix\n',
    [], [[word('ThirteenAG'), ['Widescreen Fixes Pack']], [word('TVasya'), ['NFSU2 Shadow Fix']], [word('mihaits'), ['NFSU2-SplitScreen']]]);
  assert.deepEqual([[...got.links], [...got.mods], [...got.licences]],
    [['nfsmods.xyz/mod/7328', 'github.com/tvasya/nfsu2shadowfix'], ['Widescreen Fixes Pack', 'NFSU2 Shadow Fix'], ['MIT']]);
  assert.equal(named('a ThirteenAGs b', [], [[word('ThirteenAG'), ['x']]]).mods.size, 0, 'a name as a word');
  for (const f of Object.keys(NOT_PORTS)) assert.ok(exists(f), `${f}: still there`);
});

test('every mod\'s licence file is an entry\'s', () => {
  const listed = new Set(MODS.flatMap(m => m.files));
  for (const f of fs.readdirSync(path.join(ROOT, 'licenses')).map(f => `licenses/${f}`))
    assert.ok(listed.has(f) || NOT_MOD_LICENCES.includes(f), `${f}: in its mod's entry in web/src/app/mods.ts`);
});
