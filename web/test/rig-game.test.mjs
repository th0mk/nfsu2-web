// The test tools' --game (web/tools/rig-game.mjs), checked without a server, a browser or the game:
//   node --test web/test/rig-game.test.mjs
// 1. Every tool's --help prints its own header comment and then what it has for the game: Underground 2's (the
//    default) or Most Wanted's, "none yet" with the reason until it has some; it starts nothing.
// 2. --game mw stops a tool that has nothing for Most Wanted yet, before it starts a server, a browser or a build;
//    an unknown game stops every tool.
// 3. run-suite: Underground 2's table is the same in --list and --help; Most Wanted's is empty.
// 4. requireGame: a server of the other game's module stops the tool; the same game, a module json without a game
//    (Underground 2's of before) and a server it cannot reach let it go on.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const TOOLS = ['run-suite', 'app-test', 'lan-pair', 'online-pair', 'roam-pair', 'roam-crowd', 'vault-pair', 'audio-test',
  'intro-test', 'bench', 'shader-check', 'deploy-e2e'];
// a server nobody runs, should a tool get past its --help or its check (none may): no own server, nothing reached
const NOWHERE = { 'lan-pair': ['--base', 'http://127.0.0.1:9'], 'online-pair': ['--base', 'http://127.0.0.1:9'],
  'roam-pair': ['--base', 'http://127.0.0.1:9'], 'run-suite': ['--base', 'http://127.0.0.1:9'], 'app-test': ['--base', 'http://127.0.0.1:9'],
  'audio-test': ['--base', 'http://127.0.0.1:9'], 'intro-test': ['--base', 'http://127.0.0.1:9', '--name', 'x'],
  'bench': ['--base', 'http://127.0.0.1:9'], 'shader-check': ['--port', '9'], 'roam-crowd': ['--base', 'http://127.0.0.1:9', '--seconds', '1'],
  'vault-pair': ['--base', 'http://127.0.0.1:9', '--backup', '/nonexistent.zip'], 'deploy-e2e': ['--port', '9', '--http-port', '9'] };
const tool = (name, args) => spawnSync(process.execPath, [path.join(REPO, 'web/tools', `${name}.mjs`), ...args],
  { cwd: REPO, encoding: 'utf8', timeout: 30000 });
const header = name => {
  const head = [];
  for (const l of fs.readFileSync(path.join(REPO, 'web/tools', `${name}.mjs`), 'utf8').split('\n')) {
    if (!l.startsWith('//')) break;
    head.push(l.replace(/^\/\/ ?/, ''));
  }
  return head.join('\n');
};

for (const name of TOOLS) {
  test(`${name}: --help for each game`, () => {
    let r = tool(name, ['--help', ...NOWHERE[name]]);
    assert.equal(r.status, 0, r.stderr);
    assert.ok(r.stdout.startsWith(header(name) + '\n\n'), 'the header comment first');
    assert.match(r.stdout, /--game/, 'the header names --game');
    const ug2 = r.stdout.slice(header(name).length);
    assert.match(ug2, /^\n\n.+ for NFS Underground 2 \(--game ug2\):\n {2}\S/);
    assert.doesNotMatch(ug2, /none yet/);
    // the same with --game ug2, and with --game mw: Most Wanted's, none yet and why
    assert.equal(tool(name, ['--game', 'ug2', '--help', ...NOWHERE[name]]).stdout, r.stdout);
    r = tool(name, ['--game', 'mw', '--help', ...NOWHERE[name]]);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout.slice(header(name).length), /^\n\n.+ for NFS Most Wanted \(--game mw\):\n {2}none yet\b.+\n$/);
  });

  test(`${name}: --game mw stops it before it starts anything; an unknown game stops it`, () => {
    let r = tool(name, ['--game', 'mw', ...NOWHERE[name]]);
    assert.equal(r.status, 2, r.stdout + r.stderr);
    assert.match(r.stderr, /^--game mw: none yet\b/);
    assert.equal(r.stdout, '');
    r = tool(name, ['--game', 'nfs6', '--help', ...NOWHERE[name]]);
    assert.equal(r.status, 2);
    assert.match(r.stderr, /^--game nfs6: no such game \(ug2, mw\)/);
  });
}

test('run-suite: the scenario tables', () => {
  const list = tool('run-suite', ['--list']);
  assert.equal(list.status, 0);
  assert.match(list.stdout, /^quickrace {3}quickrace\.txt {10}marker "waitmem \[0x008654a4\]==6 satisfied", 400 s$/m);
  assert.ok(tool('run-suite', ['--help']).stdout.endsWith(list.stdout.split('\n').filter(Boolean).map(l => `  ${l}`).join('\n') + '\n'));
  const mw = tool('run-suite', ['--game', 'mw', '--list']);
  assert.equal(mw.status, 0);
  assert.match(mw.stdout, /^none yet .*--script FILE/);
  // a named scenario of the other game is unknown
  const r = tool('run-suite', ['--game', 'mw', '--base', 'http://127.0.0.1:9', 'quickrace']);
  assert.equal(r.status, 2);
  assert.match(r.stderr, /unknown scenario quickrace/);
});

test('requireGame: the server must serve the tool\'s game', async t => {
  let json = '';
  const server = http.createServer((q, s) => { if (q.url === '/game.json' && json) { s.end(json); } else { s.statusCode = 404; s.end(); } });
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  t.after(() => server.close());
  const base = `http://127.0.0.1:${server.address().port}`;
  const check = (b, game) => new Promise(res => {
    const c = spawn(process.execPath, ['--input-type=module', '-e',
      `import { requireGame } from ${JSON.stringify(path.join(REPO, 'web/tools/rig-game.mjs'))}; await requireGame(${JSON.stringify(b)}, ${JSON.stringify(game)}); console.log('on');`]);
    let out = '', err = '';
    c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { err += d; });
    c.on('exit', code => res({ code, out, err }));
  });
  json = JSON.stringify({ rev: 'x', game: 'ug2' });
  assert.deepEqual(await check(base, 'ug2'), { code: 0, out: 'on\n', err: '' });
  let r = await check(base, 'mw');
  assert.equal(r.code, 2);
  assert.match(r.err, /serves the NFS Underground 2 module, not NFS Most Wanted: start the server with --game mw, or give this tool --game ug2/);
  json = JSON.stringify({ rev: 'x', game: 'mw' });
  assert.equal((await check(base, 'mw')).code, 0);
  assert.equal((await check(base, 'ug2')).code, 2);
  // a module json of before the game field: Underground 2's
  json = JSON.stringify({ rev: 'x' });
  assert.equal((await check(base, 'ug2')).code, 0);
  assert.equal((await check(base, 'mw')).code, 2);
  // no module json, or no server: left to the tool
  json = '';
  assert.equal((await check(base, 'mw')).code, 0);
  r = await check('http://127.0.0.1:9', 'mw');
  assert.equal(r.code, 0);
});
