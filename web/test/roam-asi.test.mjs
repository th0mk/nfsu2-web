// The Windows free-roam plugin, NFSU2FreeRoam.asi, still builds: tools/build_roam_asi.sh compiles the shared
// runtime/roam_core.c and roam_game.c freestanding, with win/roam_asi/roam_libc.c as their whole C library, so a C
// library function those files start to use (names.h's strstr, 8d42385) fails the plugin's link and nothing else.
// Then tools/asi_check.py on it, with the game's SPEED2.EXE (NFSU2_DATA, else ../../gamedata beside the clone, else
// the game folder the other tests use; the build script gets the same folder and checks win/roam_asi/roam_fp.h against
// it): asi_check needs the exe for the hook site and the fingerprint.  Without the exe the test shows as skipped.
//   node --test web/test/roam-asi.test.mjs        (needs mingw-w64: brew install mingw-w64)
// tools/roam_asi_test.sh runs the rest (the Unicorn harness on a free-roam snapshot).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const CC = process.env.MINGW_CC || 'i686-w64-mingw32-gcc';
const PY = path.join(REPO, '.venv/bin/python');
const haveCC = spawnSync(CC, ['--version'], { encoding: 'utf8' }).status === 0;
const EXE = [process.env.NFSU2_DATA, path.join(REPO, '../../gamedata'), path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2')]
  .filter(Boolean).map(d => path.join(d, 'SPEED2.EXE')).find(f => fs.existsSync(f)) ?? null;

test('build_roam_asi.sh links the plugin and its self-test, and asi_check.py passes it', { skip: !haveCC && `no ${CC} (brew install mingw-w64)`, timeout: 180000 }, (t) => {
  const b = spawnSync('bash', [path.join(REPO, 'tools/build_roam_asi.sh')],
    { cwd: REPO, encoding: 'utf8', timeout: 170_000, env: { ...process.env, ...(EXE ? { NFSU2_DATA: path.dirname(EXE) } : {}) } });
  const log = (b.stdout ?? '') + (b.stderr ?? '');
  assert.doesNotMatch(log, /undefined reference to `[^']+'/, 'every C library function the plugin calls is in roam_libc.c');
  assert.equal(b.status, 0, log.split('\n').slice(-8).join('\n'));
  for (const f of ['NFSU2FreeRoam.asi', 'dbg/NFSU2FreeRoam.asi', 'roam_selftest.dll', 'dist/scripts/NFSU2FreeRoam.asi'])
    assert.ok(fs.existsSync(path.join(REPO, 'out/win', f)), `out/win/${f}`);
  if (!EXE || !fs.existsSync(PY)) { t.skip(`it builds; asi_check.py did not run (${EXE ? `no ${PY}` : 'no SPEED2.EXE: NFSU2_DATA'})`); return; }
  const c = spawnSync(PY, ['-I', path.join(REPO, 'tools/asi_check.py'), path.join(REPO, 'out/win/NFSU2FreeRoam.asi'), EXE],
    { cwd: REPO, encoding: 'utf8', timeout: 120_000 });
  assert.equal(c.status, 0, (c.stdout ?? '') + (c.stderr ?? ''));
  assert.match(c.stdout, /NFSU2FreeRoam\.asi: PASS/);
});
