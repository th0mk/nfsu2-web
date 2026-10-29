// The test tools' --game (web/tools/rig-game.mjs), checked without a server, a browser or the game:
//   node --test web/test/rig-game.test.mjs
// 1. Every tool's --help prints its own header comment and then what it has for the game: Underground 2's (the
//    default) or Most Wanted's, "none yet" with the reason until it has some (run-suite has some); it starts nothing.
// 2. --game mw stops a tool that has nothing for Most Wanted yet, before it starts a server, a browser or a build;
//    an unknown game stops every tool.
// 3. run-suite: each game's table is the same in --list and --help; Most Wanted's has datafault.
// 4. requireGame: a server of the other game's module stops the tool; the same game, a module json without a game
//    (Underground 2's of before) and a server it cannot reach let it go on.
// 5. moduleLine: the module a server sends in one line (revision, game, lift, hash, and the stamp where the json
//    has one); run-suite prints it first.
// 6. run-suite --game mw with no scenario named takes Most Wanted's table; on a module with the D3DX oracle and no
//    D3DX seed it runs none of it and says why (no browser starts).
// Ports: NFSU2_TEST_RIG_PORT1 (8539) and NFSU2_TEST_RIG_PORT2 (8540), the stand-in servers of 4, 5 and 6.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const P1 = +(process.env.NFSU2_TEST_RIG_PORT1 || 8539), P2 = +(process.env.NFSU2_TEST_RIG_PORT2 || 8540);
const TOOLS = ['run-suite', 'app-test', 'lan-pair', 'online-pair', 'roam-pair', 'roam-crowd', 'vault-pair', 'audio-test',
  'intro-test', 'bench', 'shader-check', 'deploy-e2e'];
const FOR_MW = new Set(['run-suite']);   // the tools that have something for Most Wanted (3. checks what)
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
    // the same with --game ug2, and with --game mw: Most Wanted's, or none yet and why
    assert.equal(tool(name, ['--game', 'ug2', '--help', ...NOWHERE[name]]).stdout, r.stdout);
    r = tool(name, ['--game', 'mw', '--help', ...NOWHERE[name]]);
    assert.equal(r.status, 0, r.stderr);
    const mw = r.stdout.slice(header(name).length);
    if (FOR_MW.has(name)) {
      assert.match(mw, /^\n\n.+ for NFS Most Wanted \(--game mw\):\n {2}\S/);
      assert.doesNotMatch(mw, /none yet/);
    } else assert.match(mw, /^\n\n.+ for NFS Most Wanted \(--game mw\):\n {2}none yet\b.+\n$/);
  });

  test(`${name}: --game mw stops it before it starts anything${FOR_MW.has(name) ? ' (it has something)' : ''}; an unknown game stops it`, () => {
    let r;
    if (!FOR_MW.has(name)) {
      r = tool(name, ['--game', 'mw', ...NOWHERE[name]]);
      assert.equal(r.status, 2, r.stdout + r.stderr);
      assert.match(r.stderr, /^--game mw: none yet\b/);
      assert.equal(r.stdout, '');
    }
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
  assert.match(mw.stdout, /^datafault {3}boot_menu\.txt {10}marker "waitfng MainMenu\.fng satisfied", NFSU2_DATA_FAULT=fail:7,hang:29,stall:3000,deny:4,full:40, 400 s$/m);
  assert.ok(tool('run-suite', ['--game', 'mw', '--help']).stdout.endsWith(mw.stdout.split('\n').filter(Boolean).map(l => `  ${l}`).join('\n') + '\n'));
  // a named scenario of the other game is unknown
  const r = tool('run-suite', ['--game', 'mw', '--base', 'http://127.0.0.1:9', 'quickrace']);
  assert.equal(r.status, 2);
  assert.match(r.stderr, /unknown scenario quickrace/);
});

test('requireGame: the server must serve the tool\'s game', async t => {
  let json = '';
  const server = http.createServer((q, s) => { if (q.url === '/game.json' && json) { s.end(json); } else { s.statusCode = 404; s.end(); } });
  await new Promise((r, j) => { server.once('error', j); server.listen(P1, '127.0.0.1', r); });
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

test('moduleLine: the module the server sends, in one line; run-suite prints it first', async t => {
  let json = '';
  const server = http.createServer((q, s) => {
    if (q.url === '/test/hostcheck.html') { s.end('<!doctype html>'); return; }
    if (q.url === '/game.json' && json) { s.end(json); return; }
    s.statusCode = 404; s.end();
  });
  await new Promise((r, j) => { server.once('error', j); server.listen(P2, '127.0.0.1', r); });
  t.after(() => server.close());
  const base = `http://127.0.0.1:${server.address().port}`;
  const line = b => new Promise(res => {
    const c = spawn(process.execPath, ['--input-type=module', '-e',
      `import { moduleLine } from ${JSON.stringify(path.join(REPO, 'web/tools/rig-game.mjs'))}; console.log(JSON.stringify(await moduleLine(${JSON.stringify(b)})));`]);
    let out = '';
    c.stdout.on('data', d => { out += d; });
    c.on('exit', () => res(JSON.parse(out)));
  });
  const LIFT = 'FLAGABI X87OPT JTSWITCH LREGS FTOL2INL INLINE_LIST';
  // Underground 2's json (no stamp), as the server sends it (module: the hash it added)
  json = JSON.stringify({ pages: 16384, rev: '6ea741b+', size: 35666985, opt: '-O3', lift: LIFT, game: 'ug2', module: 'AbC-12_x' });
  assert.equal(await line(base), `module: rev 6ea741b+, game ug2, lift ${LIFT}, hash AbC-12_x`);
  // another game's names its lift stamp; a json of before the game field is Underground 2's
  json = JSON.stringify({ rev: 'abc', lift: '', game: 'mw', stamp: 'overrides 1 (2); exe 3', module: 'h' });
  assert.equal(await line(base), 'module: rev abc, game mw, lift -, hash h, stamp overrides 1 (2); exe 3');
  json = JSON.stringify({ rev: 'unknown+', lift: LIFT, module: 'h' });
  assert.equal(await line(base), `module: rev unknown+, game ug2 (no game field), lift ${LIFT}, hash h`);
  // no json, or no server: nothing to say
  json = '';
  assert.equal(await line(base), null);
  assert.equal(await line('http://127.0.0.1:9'), null);
  // run-suite says it before anything else (here it stops at the unknown scenario, before a browser starts)
  json = JSON.stringify({ rev: '6ea741b+', lift: LIFT, game: 'ug2', module: 'AbC-12_x' });
  const r = await new Promise(res => {
    const c = spawn(process.execPath, [path.join(REPO, 'web/tools/run-suite.mjs'), '--base', base, 'nosuchscenario'], { cwd: REPO });
    let out = '', err = '';
    c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { err += d; });
    c.on('exit', code => res({ code, out, err }));
  });
  assert.equal(r.code, 2);
  assert.equal(r.out, `module: rev 6ea741b+, game ug2, lift ${LIFT}, hash AbC-12_x\n`);
  assert.match(r.err, /unknown scenario nosuchscenario/);
});

test('run-suite --game mw: its table; on a D3DX oracle module without the D3DX seed, none of it runs', async t => {
  const server = http.createServer((q, s) => {
    if (q.url === '/test/hostcheck.html') { s.end('<!doctype html>'); return; }
    if (q.url === '/game.json') { s.end(JSON.stringify({ rev: 'abc', lift: '', game: 'mw', stamp: 'x', module: 'h', oracle: 1 })); return; }
    s.statusCode = 404; s.end();
  });
  await new Promise((r, j) => { server.once('error', j); server.listen(P1, '127.0.0.1', r); });
  t.after(() => server.close());
  const run = `rig-game-test-${process.pid}`;
  t.after(() => fs.rmSync(path.join(REPO, 'out/web/runs', run), { recursive: true, force: true }));
  const env = { ...process.env };
  delete env.NFSU2_SAVE_SEED;
  const r = await new Promise(res => {
    const c = spawn(process.execPath, [path.join(REPO, 'web/tools/run-suite.mjs'), '--game', 'mw', '--base', `http://127.0.0.1:${P1}`,
      '--run', run], { cwd: REPO, env });
    let out = '', err = '';
    c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { err += d; });
    c.on('exit', code => res({ code, out, err }));
  });
  assert.equal(r.code, 0, r.out + r.err);
  const table = tool('run-suite', ['--game', 'mw', '--list']).stdout.split('\n').filter(Boolean).map(l => l.split(' ')[0]);
  assert.ok(table.length >= 1);
  assert.match(r.out, new RegExp(`\\(NFS Most Wanted\\): 0 passed, 0 failed, ${table.length} not runnable yet\\n`));
  for (const name of table)
    assert.match(r.out, new RegExp(`^\\| ${name} \\| not runnable yet \\| - \\| - \\| this module has the D3DX oracle, .*NFSU2_SAVE_SEED=.*NFSMW_D3DX=`, 'm'));
});
