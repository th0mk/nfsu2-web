// web/start.sh in a stub tree: a failing build stops it with the build's error,
// and the server is not started (phase-2 integrate review, finding 2); a GAME
// in the shell does not change which module it builds.  The stub's
// web/build.sh, web/build.mjs, web/server-rs/build.sh and server only print and exit as told.
//   node --test web/test/start-sh.test.mjs
import { test, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const base = fs.mkdtempSync(path.join(os.tmpdir(), 'nfsu2-startsh-'));
after(() => fs.rmSync(base, { recursive: true, force: true }));

let n = 0;
/** a tree with start.sh, a module, a page and a server older than their sources */
function tree({ module = 0, page = 0, server = 0 } = {}) {
  const t = path.join(base, `t${n++}`);
  const w = (p, s, mode) => { fs.mkdirSync(path.dirname(path.join(t, p)), { recursive: true }); fs.writeFileSync(path.join(t, p), s); if (mode) fs.chmodSync(path.join(t, p), mode); };
  w('web/start.sh', fs.readFileSync(path.join(ROOT, 'web/start.sh')), 0o755);
  w('game/SPEED2.EXE', 'x');
  for (const d of ['web/node_modules/esbuild', 'deps/wasi-sdk-34']) fs.mkdirSync(path.join(t, d), { recursive: true });
  w('gen/all/fntab.c', '');
  w('out/web/nfsu2.wasm', 'old module');
  w('web/dist/main.js', 'old page');
  const stub = name => `#!/bin/bash\necho "SERVER STARTED (${name}) with $(cat out/web/nfsu2.wasm)"\n`;
  w('out/web/server-rs/nfsu2-server', stub('old'), 0o755);
  const past = new Date(Date.now() - 3600e3);
  for (const f of ['out/web/nfsu2.wasm', 'web/dist/main.js', 'out/web/server-rs/nfsu2-server']) fs.utimesSync(path.join(t, f), past, past);
  // several newer sources: the stale checks must not trip over their own pipes
  for (let i = 0; i < 300; i++) w(`runtime/f${i}.c`, '');
  for (let i = 0; i < 300; i++) w(`web/src/f${i}.ts`, '');
  for (let i = 0; i < 300; i++) w(`web/server-rs/crates/c/src/f${i}.rs`, '');
  const errs = Array.from({ length: 30 }, (_, i) => `error line ${i + 1}`).join('\n');
  w('web/build.sh', `#!/bin/bash\necho compiling\n${module ? `echo "${errs}" >&2\nexit ${module}` : 'echo "link OK: out/web/nfsu2.wasm (GAME=${GAME:-none})"; echo new > out/web/nfsu2.wasm'}\n`, 0o755);
  w('web/build.mjs', page ? `console.error(${JSON.stringify(errs.replace(/error/g, 'page error'))}); process.exit(${page});\n` : `console.log('page built');\n`);
  w('web/server-rs/build.sh', `#!/bin/bash\necho compiling\n${server ? `echo "${errs.replace(/error/g, 'server error')}" >&2\nexit ${server}`
    : `printf '%s' '${stub('new')}' > out/web/server-rs/nfsu2-server; echo out/web/server-rs/nfsu2-server`}\n`, 0o755);
  return t;
}
const run = (t, env = {}) => {
  const r = spawnSync('bash', [path.join(t, 'web/start.sh'), '--data', path.join(t, 'game'), '--http-port', '48080'],
    { cwd: t, encoding: 'utf8', timeout: 60000, env: { ...process.env, ...env } });
  return { code: r.status, out: (r.stdout ?? '') + (r.stderr ?? '') };
};

test('a module build that fails stops start.sh with its error; no server', () => {
  const r = run(tree({ module: 1 }));
  assert.notEqual(r.code, 0);
  assert.match(r.out, /error line 1\b/, 'more than the last 3 lines of the error');
  assert.match(r.out, /error line 30/);
  assert.match(r.out, /The game module did not build/);
  assert.doesNotMatch(r.out, /SERVER STARTED|Starting the server/);
});

test('a page build that fails stops start.sh with its error; no server', () => {
  const r = run(tree({ page: 2 }));
  assert.notEqual(r.code, 0);
  assert.match(r.out, /page error line 1\b/);
  assert.match(r.out, /The page did not build/);
  assert.doesNotMatch(r.out, /SERVER STARTED|Starting the server/);
});

test('a server build that fails stops start.sh with its error; no server', () => {
  const r = run(tree({ server: 101 }));
  assert.notEqual(r.code, 0);
  assert.match(r.out, /server error line 1\b/);
  assert.match(r.out, /The server did not build/);
  assert.doesNotMatch(r.out, /SERVER STARTED|Starting the server/);
});

test('builds that succeed start the new server with the new module', () => {
  const r = run(tree());
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /link OK/);
  assert.match(r.out, /page built/);
  assert.match(r.out, /SERVER STARTED \(new\) with new/);
});

test('a GAME left in the shell (GAME=mw) still builds UG2\'s module, the one start.sh serves', () => {
  for (const env of [{ GAME: 'mw' }, { GAME: 'ug2' }]) {
    const r = run(tree(), env);
    assert.equal(r.code, 0, r.out);
    assert.match(r.out, /link OK: out\/web\/nfsu2\.wasm \(GAME=ug2\)/);
    assert.match(r.out, /SERVER STARTED \(new\) with new/);
  }
});
