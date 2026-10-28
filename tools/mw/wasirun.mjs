// Runs a wasm32-wasip1 program under node's WASI with one host directory as /fx (tools/mw/fxdump.sh --wasm: A's
// preshader tests and runner linked for wasm, docs/mw/fx-preshader.md §6.6).  The exit status is the program's.
//   node tools/mw/wasirun.mjs <program.wasm> <host dir> [args...]
import { WASI } from 'node:wasi';
import fs from 'node:fs';

const [wasm, dir, ...args] = process.argv.slice(2);
const wasi = new WASI({ version: 'preview1', args: [wasm, ...args], preopens: { '/fx': dir }, returnOnExit: true });
const inst = await WebAssembly.instantiate(await WebAssembly.compile(fs.readFileSync(wasm)), wasi.getImportObject());
process.exitCode = wasi.start(inst);
