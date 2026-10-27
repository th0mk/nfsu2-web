// Runs a small wasm32-wasip1-threads program under Node's WASI (no threads
// started): node web/test/wasi-run.mjs <program.wasm> [args...]
// Memory is imported (shared, 64 MB .. 256 MB); the runtime's host imports
// ("nfsu2") are stubs.  WASI_DIR=<guest path>=<host dir> gives the program
// one directory (none by default).
import fs from 'node:fs';
import { WASI } from 'node:wasi';
const [file, ...args] = process.argv.slice(2);
const mod = await WebAssembly.compile(fs.readFileSync(file));
const memory = new WebAssembly.Memory({ initial: 1024, maximum: 4096, shared: true });
const dir = process.env.WASI_DIR?.split('=');
const preopens = dir?.length === 2 ? { [dir[0]]: dir[1] } : {};
const wasi = new WASI({ version: 'preview1', args: [file, ...args], env: {}, preopens, returnOnExit: true });
const inst = await WebAssembly.instantiate(mod, {
  env: { memory }, wasi_snapshot_preview1: wasi.wasiImport, wasi: { 'thread-spawn': () => -1 },
  nfsu2: { register: () => 0, now_ms: () => performance.timeOrigin + performance.now(), blocked_ms: () => 0 },
});
process.exitCode = wasi.start(inst);
