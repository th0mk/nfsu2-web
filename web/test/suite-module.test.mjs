// web/tools/run-suite.mjs names the module it ran against (a result counts only for the module that made it),
// checked against a stand-in server, without a browser or the game:
//   node --test web/test/suite-module.test.mjs
// run-suite prints the module line (moduleLine, web/tools/rig-game.mjs: the server's /game.json in one line; its own
// cases are in rig-game.test.mjs) before anything else, and writes it into summary.txt and summary.json, also when
// no scenario it was given can run yet (then without starting a browser).
// Port: NFSU2_TEST_SUITE_PORT (8541), the stand-in's.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const LIFT = 'FLAGABI X87OPT JTSWITCH LREGS FTOL2INL INLINE_LIST';
const PORT = +(process.env.NFSU2_TEST_SUITE_PORT || 8541);

/** a stand-in for the server with --dev: the test page, and /game.json while `json()` gives one */
async function standIn(t, json) {
  const server = http.createServer((q, s) => {
    if (q.url === '/test/hostcheck.html') { s.end('<!doctype html>'); return; }
    if (q.url === '/game.json' && json()) { s.end(json()); return; }
    s.statusCode = 404; s.end();
  });
  await new Promise((r, j) => { server.once('error', j); server.listen(PORT, '127.0.0.1', r); });
  t.after(() => server.close());
  return `http://127.0.0.1:${server.address().port}`;
}

/** a node process: its exit code and what it printed */
const node = (args, env = process.env) => new Promise(res => {
  const c = spawn(process.execPath, args, { cwd: REPO, env });
  let out = '', err = '';
  c.stdout.on('data', d => { out += d; }); c.stderr.on('data', d => { err += d; });
  c.on('exit', code => res({ code, out, err }));
});

test('run-suite: the module line first, and in both summaries', async t => {
  const json = JSON.stringify({ rev: '4eda10d+', lift: LIFT, module: 'AbC-12_x' });
  const base = await standIn(t, () => json);
  // (a json of before the game field: Underground 2's)
  const want = `module: rev 4eda10d+, game ug2 (no game field), lift ${LIFT}, hash AbC-12_x`;
  // before anything else (here it stops at the unknown scenario, before a browser starts)
  let r = await node([path.join(REPO, 'web/tools/run-suite.mjs'), '--base', base, 'nosuchscenario']);
  assert.equal(r.code, 2);
  assert.equal(r.out, `${want}\n`);
  assert.match(r.err, /unknown scenario nosuchscenario/);
  // a run of a scenario that is not runnable yet (outrun without a save seed): nothing to start, the summaries written
  const run = `suite-module-test-${process.pid}`, out = path.join(REPO, 'out/web/runs', run);
  t.after(() => fs.rmSync(out, { recursive: true, force: true }));
  const env = { ...process.env }; delete env.NFSU2_SAVE_SEED;
  r = await node([path.join(REPO, 'web/tools/run-suite.mjs'), '--base', base, '--run', run, 'outrun'], env);
  assert.equal(r.code, 0, r.err);
  assert.ok(r.out.startsWith(`${want}\n`), r.out);
  const sum = fs.readFileSync(path.join(out, 'summary.txt'), 'utf8').split('\n');
  assert.equal(sum[0], `web suite ${run}: 0 passed, 0 failed, 1 not runnable yet`);
  assert.equal(sum[1], want);
  const j = JSON.parse(fs.readFileSync(path.join(out, 'summary.json'), 'utf8'));
  assert.equal(j.module, want);
  assert.deepEqual(j.skipped.map(s => s.name), ['outrun']);
});
