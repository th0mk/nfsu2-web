// The page's unit tests (web/test/*.test.mjs) under node --test, every test and suite with a time limit (a hang fails
// that test instead of holding the run: 2 minutes with --fast, whose tests take seconds; 5 for every file, as
// deploy.test.mjs's suite takes about 80 s):
//   node web/tools/unit-tests.mjs           every file (about 80 s)
//   node web/tools/unit-tests.mjs --fast    without the slow ones (about 11 s): before a push, and in CI
// Other arguments go to node --test (--test-reporter=spec ...).  Also npm test / npm run test:fast in web/.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TEST = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../test');
// the slow ones: each waits on the server's clocks, a browser or a C build
const SLOW = new Set([
  'deploy',        // ~80 s: the keep-alive, the certificate check and the proxy idle cut, on the server's own clocks
  'audit',         // ~36 s: F3 holds hundreds of requests
  'page-narrow',   // ~26 s: Chrome, the page in every language
  'data-blocks',   // ~27 s: brotli at its best
  'lan-wasm',      // ~17 s: lan_unit.c built for wasm
  'roam-asi',      // ~11 s: the Windows plugin built with mingw
]);
const argv = process.argv.slice(2);
const fast = argv.includes('--fast');
const files = fs.readdirSync(TEST).filter(f => f.endsWith('.test.mjs') && !(fast && SLOW.has(f.slice(0, -'.test.mjs'.length))))
  .sort().map(f => path.join(TEST, f));
const r = spawnSync(process.execPath, ['--test', `--test-timeout=${fast ? 120_000 : 300_000}`, ...argv.filter(a => a !== '--fast'), ...files], { stdio: 'inherit' });
process.exit(r.status ?? 1);
