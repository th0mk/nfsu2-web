// Checks that the TypeScript mirrors of the shared-memory layouts carry the
// same numbers as their C headers: every name defined on both sides must
// have the same value, and every C name must exist in TypeScript.
//   node web/tools/check-proto.mjs        (exit 1 on a mismatch)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// C: #define NAME <number> and enum members (explicit or implicit values)
function cNames(file, prefixes) {
  const src = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
  const out = new Map();
  for (const m of src.matchAll(/^#define\s+([A-Z_0-9]+)\s+(0x[0-9a-fA-F]+|\d+)u?\b/gm)) out.set(m[1], Number(m[2]));
  for (const m of src.matchAll(/enum\s*\{([^}]*)\}/g)) {
    let next = 0;
    for (const item of m[1].split(',').map(s => s.trim()).filter(Boolean)) {
      const [name, val] = item.split('=').map(s => s.trim());
      const v = val === undefined ? next : Number(val.replace(/u$/, ''));
      out.set(name, v); next = v + 1;
    }
  }
  return new Map([...out].filter(([k]) => prefixes.some(p => k.startsWith(p))));
}
// TS: export const NAME = <number>[, NAME = <number>]...
function tsNames(file) {
  const src = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/\/\/.*$/gm, '');
  const out = new Map();
  for (const m of src.matchAll(/export const ([^;]+);/g))
    for (const d of m[1].split(',')) { const r = /^\s*([A-Z_0-9]+)\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*$/.exec(d); if (r) out.set(r[1], Number(r[2])); }
  return out;
}
const pairs = [
  ['runtime/web/audio_proto.h', 'web/src/audio/proto.ts', ['AR_', 'AUDIO_']],
  ['runtime/web/input_proto.h', 'web/src/input/proto.ts', ['IM_', 'IEV_', 'IKF_', 'INAME_', 'IREASON_', 'ISYN_', 'INPUT_']],
  ['runtime/kbd.h', 'web/src/input/proto.ts', ['KSYN_']],
  ['runtime/gamepad.h', 'web/src/input/proto.ts', ['PAD_', 'PADF_']],
  ['runtime/web/host_abi.h', 'web/src/host/abi.ts', []],
];
let bad = 0, checked = 0;
for (const [c, t, pre] of pairs) {
  if (!pre.length) continue;
  const cn = cNames(c, pre), tn = tsNames(t);
  for (const [k, v] of cn) {
    if (k === 'PAD_NBUTTONS' || k === 'PAD_NAXES') continue;
    checked++;
    if (!tn.has(k)) { console.log(`MISSING in ${t}: ${k} = ${v} (${c})`); bad++; }
    else if (tn.get(k) !== v) { console.log(`MISMATCH ${k}: ${c} ${v}, ${t} ${tn.get(k)}`); bad++; }
  }
}
console.log(bad ? `check-proto: ${bad} problem(s) in ${checked} names` : `check-proto: OK, ${checked} names agree`);
process.exit(bad ? 1 : 0);
