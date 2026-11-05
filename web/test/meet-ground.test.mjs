// Collision placement policy without proprietary game data; also runs in Linux CI.
import test from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const repo = fileURLToPath(new URL('../../', import.meta.url));
test('meet spectators reject unsafe surfaces and retry streamed geometry', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'meet-ground-'));
  try {
    const exe = path.join(dir, 'meet-ground');
    execFileSync('cc', ['-std=c11', '-O2', '-Wall', '-Wextra', '-Iruntime', 'tests/meet_ground_unit.c', '-lm', '-o', exe], { cwd: repo });
    assert.match(execFileSync(exe, { encoding: 'utf8' }), /PASS:/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
