#!/usr/bin/env node
// Compare two device-call recordings (NFSU2_DXREC, runtime/comrec.c; docs/web/dxrec.md) of one scripted run:
// the D3DX oracle's and the HLE's, or two runs of one build (the noise floor, L-082).
//
//   node web/tools/dxrec-diff.mjs A.txt B.txt [--show N] [--quiet]
//
// The frames ("F <n>" lines) are paired first, by what the game drew in them: each frame's key is its Draw*, Clear
// and Present lines, and Myers' diff over the two key sequences pairs the frames whose draws are the same (a run
// that took longer to load has frames the other has not: "unpaired", the timing noise of a scripted run).  Then,
// for each pair:
//   calls  the device calls as written, in order (Myers' diff): each call only one side made is a difference.
//          The game's context lines ("E<id> ..." from runtime/mw/d3dxfx_mw.c and fxcensus_mw.c) are shown
//          around a difference but never compared.
//   draws  each file is replayed into the device state com.c keeps (render, stage and sampler states, textures,
//          shaders, constants, declaration, streams, transforms, lights, viewport, targets), with its state
//          blocks as com.c records, captures and applies them; the state each Draw* call sees is compared draw
//          by draw (an order-free hash of the whole state; the differing states are listed for the first ones).
// Prints the totals, the first N differences of each kind (default 6) and "result: <calls> call differences,
// <draws> draw differences in <paired> paired frames, <unpaired> unpaired"; exits 0 when calls and draws are 0.
import fs from 'node:fs';

const args = process.argv.slice(2);
const opt = { show: 6, quiet: false };
const files = [];
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--show') opt.show = +args[++i];
  else if (a === '--quiet') opt.quiet = true;
  else files.push(a);
}
if (files.length !== 2) { console.error('usage: dxrec-diff.mjs A.txt B.txt [--show N] [--quiet]'); process.exit(2); }

const isMark = s => /^E(-|\d)/.test(s);          // "E<id> ..." and "E- ..." are context lines
const isKey = s => s.startsWith('Draw') || s.startsWith('Clear ') || s.startsWith('Present ');
function fnv(s, h = 2166136261) {
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619) >>> 0; }
  return h >>> 0;
}

// ---- reading: frames of lines ----
// Objects are renumbered per kind in the order the file first names them: two runs that made their buffers,
// textures or state blocks in another order (a streaming load's timing), or recordings of a window of Presents,
// name the same objects alike when they use them in the same order.
function read(file) {
  const text = fs.readFileSync(file, 'utf8');
  const frames = [];
  const seen = new Map(), next = new Map();
  const renum = l => l.indexOf('#') < 0 ? l : l.replace(/\b([a-z]+)#(\d+)\b/g, (m, k, n) => {
    const key = `${k}#${n}`;
    if (!seen.has(key)) { const c = (next.get(k) ?? 0) + 1; next.set(k, c); seen.set(key, `${k}#${c}`); }
    return seen.get(key);
  });
  let cur = { no: 0, lines: [] };
  for (let line of text.split('\n')) {
    if (!line || line.startsWith('dxrec ')) continue;
    line = renum(line);
    if (line.startsWith('F ')) { frames.push(cur); cur = { no: +line.slice(2), lines: [] }; continue; }
    cur.lines.push(line);
  }
  frames.push(cur);
  for (const f of frames) {
    f.calls = f.lines.filter(l => !isMark(l));
    // the draws without their vertex data: a frame whose geometry moved (a loading bar's) still pairs, and its
    // draws then differ
    f.key = f.calls.filter(isKey).map(l => l.replace(/ #[0-9a-f]{8}\/\d+/g, '').replace(/\b[a-z]+#\d+\b/g, 'obj')).join('\n');
  }
  return frames;
}

// ---- Myers' diff: the edit script (bounded: past cap edits, null) ----
function myers(a, b, cap) {
  const n = a.length, m = b.length, max = n + m, off = max + 1;
  if (!max) return [];
  const v = new Int32Array(2 * max + 3), trace = [];
  let found = -1;
  for (let d = 0; d <= Math.min(max, cap); d++) {
    trace.push(v.slice(off - d - 1, off + d + 2));
    for (let k = -d; k <= d; k += 2) {
      let x = (k === -d || (k !== d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
      let y = x - k;
      while (x < n && y < m && a[x] === b[y]) { x++; y++; }
      v[off + k] = x;
      if (x >= n && y >= m) { found = d; break; }
    }
    if (found >= 0) break;
  }
  if (found < 0) return null;
  const ops = []; let x = n, y = m;
  for (let d = found; d > 0; d--) {
    const t = trace[d], base = -d - 1;                // t[i] holds v[k] for k = i + base
    const V = k => t[k - base];
    const k = x - y;
    const prevK = (k === -d || (k !== d && V(k - 1) < V(k + 1))) ? k + 1 : k - 1;
    const px = V(prevK), py = px - prevK;
    while (x > px && y > py) { x--; y--; ops.push({ op: '=', i: x, j: y }); }
    if (x === px) { y--; ops.push({ op: '+', j: y }); } else { x--; ops.push({ op: '-', i: x }); }
  }
  while (x > 0 && y > 0) { x--; y--; ops.push({ op: '=', i: x, j: y }); }
  return ops.reverse();
}
// the edit distance only, in linear memory (bounded: past cap, n + m)
function distance(a, b, cap) {
  const n = a.length, m = b.length, max = n + m, off = max + 1;
  if (!max) return 0;
  const v = new Int32Array(2 * max + 3);
  for (let d = 0; d <= Math.min(max, cap); d++) {
    for (let k = -d; k <= d; k += 2) {
      let x = (k === -d || (k !== d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
      let y = x - k;
      while (x < n && y < m && a[x] === b[y]) { x++; y++; }
      v[off + k] = x;
      if (x >= n && y >= m) return d;
    }
  }
  return n + m;
}

// ---- the device state as com.c keeps it (runtime/com.c sb_recordable, Capture) ----
const RECORDABLE = new Set(['SetRenderState', 'SetTextureStageState', 'SetSamplerState', 'SetTexture',
  'SetVertexShader', 'SetPixelShader', 'SetVertexDeclaration', 'SetFVF', 'SetVertexShaderConstantF',
  'SetPixelShaderConstantF', 'SetVertexShaderConstantI', 'SetVertexShaderConstantB', 'SetTransform', 'SetMaterial',
  'SetLight', 'LightEnable', 'SetStreamSource', 'SetIndices', 'SetViewport', 'SetScissorRect', 'SetNPatchMode',
  'SetClipPlane']);
const SETTERS = new Set([...RECORDABLE, 'SetRenderTarget', 'SetDepthStencilSurface']);
class Device {
  constructor() { this.s = new Map(); this.h1 = 0; this.h2 = 0; this.rec = null; this.blocks = new Map(); }
  set(k, v) {
    const old = this.s.get(k);
    if (old === v) return;
    if (old !== undefined) { const e = `${k}=${old}`; this.h1 ^= fnv(e); this.h2 ^= fnv(e, 0x811c9dc5 ^ 0x5bd1e995); }
    const e = `${k}=${v}`; this.h1 ^= fnv(e); this.h2 ^= fnv(e, 0x811c9dc5 ^ 0x5bd1e995);
    this.s.set(k, v);
  }
  hash() { return `${this.h1.toString(16)}:${this.h2.toString(16)}`; }
  apply(w) {
    const m = w[0];
    switch (m) {
      case 'SetRenderState': this.set(`rs ${w[1]}`, w[2]); break;
      case 'SetTextureStageState': this.set(`tss ${w[1]} ${w[2]}`, w[3]); break;
      case 'SetSamplerState': this.set(`ss ${w[1]} ${w[2]}`, w[3]); break;
      case 'SetTexture': this.set(`tex ${w[1]}`, w[2]); break;
      case 'SetVertexShader': this.set('vs', w[1]); break;
      case 'SetPixelShader': this.set('ps', w[1]); break;
      case 'SetVertexDeclaration': this.set('decl', w[1]); break;
      case 'SetFVF': this.set('fvf', w[1]); break;
      case 'SetVertexShaderConstantF': case 'SetPixelShaderConstantF': case 'SetVertexShaderConstantI': {
        const kind = m === 'SetPixelShaderConstantF' ? 'psc' : m === 'SetVertexShaderConstantI' ? 'vsi' : 'vsc';
        const start = parseInt(w[1], 16), cnt = parseInt(w[3], 16);
        const d = w[2].startsWith('[') ? w[2].slice(1, -1).split(',') : null;
        for (let r = 0; r < cnt; r++) this.set(`${kind} ${start + r}`, d ? d.slice(4 * r, 4 * r + 4).join(',') : `${w[2]}@${r}`);
        break;
      }
      case 'SetVertexShaderConstantB': this.set(`vsb ${w[1]}`, w.slice(2).join(' ')); break;
      case 'SetTransform': this.set(`xf ${w[1]}`, w[2]); break;
      case 'SetMaterial': this.set('mat', w[1]); break;
      case 'SetLight': this.set(`light ${w[1]}`, w[2]); break;
      case 'LightEnable': this.set(`lighton ${w[1]}`, w[2]); break;
      case 'SetStreamSource': this.set(`stream ${w[1]}`, w.slice(2).join(' ')); break;
      case 'SetIndices': this.set('ib', w[1]); break;
      case 'SetViewport': this.set('vp', w[1]); break;
      case 'SetScissorRect': this.set('scissor', w[1]); break;
      case 'SetNPatchMode': this.set('npatch', w[1]); break;
      case 'SetClipPlane': this.set(`clip ${w[1]}`, w[2]); break;
      case 'SetRenderTarget': this.set(`rt ${w[1]}`, w[2]); break;
      case 'SetDepthStencilSurface': this.set('ds', w[1]); break;
      default: break;
    }
  }
  // one call of the stream; true when it is a draw
  call(line) {
    const w = line.split(' '); const m = w[0];
    if (m === 'BeginStateBlock') { if (!w.some(t => t.startsWith('=')) && !this.rec) this.rec = []; return false; }
    if (m === 'EndStateBlock') {
      const i = w.indexOf('->');
      if (i > 0 && this.rec) this.blocks.set(w[i + 1], this.rec);
      this.rec = null; return false;
    }
    if (m === 'Capture') { const b = this.blocks.get(w[1]); if (b) for (const r of b) this.capture(r); return false; }
    if (m === 'Apply') { const b = this.blocks.get(w[1]); if (b) for (const r of b) this.apply(r); return false; }
    if (this.rec && RECORDABLE.has(m)) { this.rec.push(w.slice()); return false; }
    if (SETTERS.has(m)) { this.apply(w); return false; }
    return m.startsWith('Draw');
  }
  // com.c's Capture: the current value into a recorded call of these kinds
  capture(r) {
    const g = k => this.s.get(k);
    const put = (i, k, lim) => { if ((lim === undefined || parseInt(r[1], 16) < lim) && g(k) !== undefined) r[i] = g(k); };
    switch (r[0]) {
      case 'SetRenderState': put(2, `rs ${r[1]}`); break;
      case 'SetTextureStageState': put(3, `tss ${r[1]} ${r[2]}`, 8); break;
      case 'SetSamplerState': put(3, `ss ${r[1]} ${r[2]}`, 16); break;
      case 'SetTexture': put(2, `tex ${r[1]}`, 8); break;
      case 'SetVertexShader': r[1] = g('vs') ?? '0'; break;          // com.c's state starts at 0
      case 'SetPixelShader': r[1] = g('ps') ?? '0'; break;
      case 'SetVertexDeclaration': r[1] = g('decl') ?? '0'; break;
      case 'SetFVF': r[1] = g('fvf') ?? '0'; break;
      default: break;
    }
  }
}
// the lights and the material reach no draw while LIGHTING (render state 0x89) is off; d3dx9_26 sets a light from
// memory it never wrote (a technique's light copy before the Begin that zeroes it), which the HLE cannot repeat, so
// they are compared only where lighting is on
function lit(m) { return m.get('rs 89') !== undefined && m.get('rs 89') !== '0'; }
function drawState(dev) {
  if (lit(dev.s)) return dev.hash();
  let h1 = dev.h1, h2 = dev.h2;
  for (const [k, v] of dev.s)
    if (k.startsWith('light') || k === 'mat') { const e = `${k}=${v}`; h1 ^= fnv(e); h2 ^= fnv(e, 0x811c9dc5 ^ 0x5bd1e995); }
  return `${h1.toString(16)}:${h2.toString(16)}`;
}
function stateDiff(a, b) {
  const out = [];
  const skip = k => (k.startsWith('light') || k === 'mat') && !lit(a) && !lit(b);
  for (const [k, v] of a) if (b.get(k) !== v && !skip(k)) out.push(`${k}: ${v} | ${b.get(k) ?? '-'}`);
  for (const [k, v] of b) if (!a.has(k) && !skip(k)) out.push(`${k}: - | ${v}`);
  return out;
}

// ---- the run ----
const A = read(files[0]), B = read(files[1]);
// pair the frames by what was drawn in them
const keyId = new Map(); const kid = k => { if (!keyId.has(k)) keyId.set(k, keyId.size); return keyId.get(k); };
const ka = A.map(f => kid(f.key)), kb = B.map(f => kid(f.key));
const fops = myers(ka, kb, 100000) ?? [];
const pairOf = new Map();                  // A frame index -> B frame index
for (const o of fops) if (o.op === '=') pairOf.set(o.i, o.j);
const pairedB = new Set(pairOf.values());
const devA = new Device(), devB = new Device();
let paired = 0, callsA = 0, callsB = 0, callDiff = 0, framesDiff = 0, drawsA = 0, drawsB = 0, drawDiff = 0;
const shownCalls = [], shownDraws = [];
function replay(dev, f, keep) {
  const out = [];
  for (const l of f.calls) if (dev.call(l)) out.push({ l, h: drawState(dev), s: keep ? new Map(dev.s) : null });
  return out;
}
// walk both streams in order, replaying every frame (the state carries over), comparing the pairs
let j = 0;
for (let i = 0; i < A.length; i++) {
  const pj = pairOf.get(i);
  if (pj === undefined) { replay(devA, A[i], false); continue; }
  while (j < pj) { replay(devB, B[j], false); j++; }
  const keep = shownDraws.length < opt.show;
  const da = replay(devA, A[i], keep), db = replay(devB, B[j], keep);
  const fa = A[i], fb = B[j]; j++;
  paired++; callsA += fa.calls.length; callsB += fb.calls.length; drawsA += da.length; drawsB += db.length;
  if (fa.calls.length !== fb.calls.length || fa.calls.some((l, k) => l !== fb.calls[k])) {
    const d = distance(fa.calls, fb.calls, 3000);
    if (d) {
      framesDiff++; callDiff += d;
      if (shownCalls.length < opt.show) {
        const ops = myers(fa.lines, fb.lines, 3000);
        shownCalls.push({ a: fa.no, b: fb.no, d, ops, la: fa.lines, lb: fb.lines });
      }
    }
  }
  for (let k = 0; k < Math.max(da.length, db.length); k++) {
    const x = da[k], y = db[k];
    if (x && y && x.h === y.h && x.l === y.l) continue;
    drawDiff++;
    if (shownDraws.length < opt.show)
      shownDraws.push({ a: fa.no, b: fb.no, k, la: x?.l, lb: y?.l, d: x?.s && y?.s ? stateDiff(x.s, y.s) : ['(a draw only one side made)'] });
  }
}
while (j < B.length) { replay(devB, B[j], false); j++; }
const unA = A.length - paired, unB = B.length - paired;
console.log(`A ${files[0]}\nB ${files[1]}`);
console.log(`frames: A ${A.length}, B ${B.length}; ${paired} paired by their draws, unpaired A ${unA}, B ${unB}`);
console.log(`in the paired frames: calls A ${callsA}, B ${callsB}; draws A ${drawsA}, B ${drawsB}`);
console.log(`calls: ${callDiff} differences in ${framesDiff} frames`);
console.log(`draws: ${drawDiff} draws whose state differs`);
if (!opt.quiet) {
  for (const s of shownCalls) {
    console.log(`\n-- frames ${s.a} | ${s.b}: ${s.d} call differences; the first ones in context (- A only, + B only):`);
    if (!s.ops) { console.log('  (too far apart to list)'); continue; }
    let shown = 0, last = -10;
    for (let q = 0; q < s.ops.length && shown < 14; q++) {
      const o = s.ops[q];
      if (o.op === '=') continue;
      const line = o.op === '-' ? s.la[o.i] : s.lb[o.j];
      if (isMark(line)) continue;
      if (q - last > 1) for (let c = Math.max(0, q - 3); c < q; c++) if (s.ops[c].op === '=') console.log(`    ${s.la[s.ops[c].i].slice(0, 150)}`);
      console.log(`  ${o.op} ${line.slice(0, 150)}`); shown++; last = q;
    }
  }
  for (const s of shownDraws) {
    console.log(`\n-- frames ${s.a} | ${s.b} draw ${s.k}: A ${s.la ?? '-'} | B ${s.lb ?? '-'}; ${s.d.length} states differ (A | B):`);
    for (const d of s.d.slice(0, 12)) console.log(`  ${d.slice(0, 200)}`);
  }
}
console.log(`result: ${callDiff} call differences, ${drawDiff} draw differences in ${paired} paired frames, ${unA}/${unB} unpaired`);
process.exit(callDiff || drawDiff ? 1 : 0);
