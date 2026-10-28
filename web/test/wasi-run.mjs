// Runs a small wasm32-wasip1-threads program under Node's WASI (no threads
// started): node web/test/wasi-run.mjs <program.wasm> [args...]
// Memory is imported (shared, 64 MB .. 256 MB, or WASI_PAGES .. WASI_MAX_PAGES
// pages of 64 KB); the runtime's host imports ("nfsu2") are stubs.
// WASI_DIR=<guest path>=<host dir> gives the program one directory (none by
// default), WASI_DIR2 a second one.
import fs from 'node:fs';
import { WASI } from 'node:wasi';
const [file, ...args] = process.argv.slice(2);
const mod = await WebAssembly.compile(fs.readFileSync(file));
const memory = new WebAssembly.Memory({ initial: +(process.env.WASI_PAGES ?? 1024), maximum: +(process.env.WASI_MAX_PAGES ?? Math.max(4096, +(process.env.WASI_PAGES ?? 0))), shared: true });
const preopens = {};
for (const v of [process.env.WASI_DIR, process.env.WASI_DIR2]) {
  const i = v ? v.indexOf('=') : -1;
  if (i > 0) preopens[v.slice(0, i)] = v.slice(i + 1);
}
const wasi = new WASI({ version: 'preview1', args: [file, ...args], env: {}, preopens, returnOnExit: true });
const inst = await WebAssembly.instantiate(mod, {
  env: { memory }, wasi_snapshot_preview1: wasi.wasiImport, wasi: { 'thread-spawn': () => -1 },
  nfsu2: { register: () => 0, now_ms: () => performance.timeOrigin + performance.now(), blocked_ms: () => 0 },
});
process.exitCode = wasi.start(inst);
