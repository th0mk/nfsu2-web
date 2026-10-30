#!/usr/bin/env node
// Compare two device-call recordings (NFSU2_DXREC, runtime/comrec.c; docs/web/dxrec.md) of one scripted run:
// the D3DX oracle's and the HLE's, or two runs of one build (the noise floor, L-082).
//
//   node web/tools/dxrec-diff.mjs A.txt B.txt [--show N] [--frames LO-HI] [--quiet]
//
// Two measures, frame by frame ("F <n>" lines; frames that only one file has are counted, not compared):
//   calls  the device calls as written, in order (Myers' diff per frame): each call only one side made is a
//          difference.  The game's context lines ("E..." from runtime/mw/d3dxfx_mw.c and fxcensus_mw.c) are
//          shown around a difference but never compared.
//   draws  each file replayed into the device state com.c keeps (render, stage and sampler states, textures,
//          shaders, constants, declaration, streams, transforms, lights, viewport, targets), with its state
//          blocks as com.c records, captures and applies them; the state every Draw* call sees is compared
//          draw by draw.  A draw only one side made is a difference too.
// Prints the totals, the first N differences of each kind (default 8) and "result: <calls> call
// differences, <draws> draw differences"; exits 0 when both are 0, else 1.
import fs from 'node:fs';

const args = process.argv.slice(2);
const opt = { show: 8, lo: 0, hi: Infinity, quiet: false };
const files = [];
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--show') opt.show = +args[++i];
  else if (a === '--frames') { const [l, h] = args[++i].split('-'); opt.lo = +l; opt.hi = h === undefined ? +l : +h; }
  else if (a === '--quiet') opt.quiet = true;
  else files.push(a);
}
if (files.length !== 2) { console.error('usage: dxrec-diff.mjs A.txt B.txt [--show N] [--frames LO-HI] [--quiet]'); process.exit(2); }

// ---- reading: frames of lines ----
function read(file) {
  const text = fs.readFileSync(file, 'utf8');
  const frames = new Map();
  let cur = [], no = 0;
  frames.set(0, cur);
  for (const line of text.split('\n')) {
    if (!line || line.startsWith('dxrec ')) continue;
    if (line.startsWith('F ')) { no = +line.slice(2); cur = []; frames.set(no, cur); continue; }
    cur.push(line);
  }
  return frames;
}

// ---- Myers' diff (the edit script of two arrays of strings) ----
function diff(a, b) {
  const n = a.length, m = b.length, max = n + m, off = max + 1;
  const v = new Int32Array(2 * max + 3); const trace = [];
  if (max === 0) return [];
  // past this many edits the frame is reported as wholly different (bounded time)
  const cap = Math.min(max, 4000);
  let found = -1;
  for (let d = 0; d <= cap; d++) {
    trace.push(v.slice());
    for (let k = -d; k <= d; k += 2) {
      let x = (k === -d || (k !== d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
      let y = x - k;
      while (x < n && y < m && a[x] === b[y]) { x++; y++; }
      v[off + k] = x;
      if (x >= n && y >= m) { found = d; break; }
    }
    if (found >= 0) break;
  }
  if (found < 0) {                       // too far apart: every line is a difference
    return [...a.map((s, i) => ({ op: '-', i, s })), ...b.map((s, j) => ({ op: '+', j, s }))];
  }
  const ops = []; let x = n, y = m;
  for (let d = found; d > 0; d--) {
    const vv = trace[d]; const k = x - y;
    const prevK = (k === -d || (k !== d && vv[off + k - 1] < vv[off + k + 1])) ? k + 1 : k - 1;
    const px = vv[off + prevK], py = px - prevK;
    while (x > px && y > py) { x--; y--; }
    if (x === px) { y--; ops.push({ op: '+', j: y, s: b[y] }); } else { x--; ops.push({ op: '-', i: x, s: a[x] }); }
  }
  return ops.reverse();
}

// ---- the device state as com.c keeps it (runtime/com.c sb_recordable, Capture) ----
const RECORDABLE = new Set(['SetRenderState', 'SetTextureStageState', 'SetSamplerState', 'SetTexture',
  'SetVertexShader', 'SetPixelShader', 'SetVertexDeclaration', 'SetFVF', 'SetVertexShaderConstantF',
  'SetPixelShaderConstantF', 'SetVertexShaderConstantI', 'SetVertexShaderConstantB', 'SetTransform', 'SetMaterial',
  'SetLight', 'LightEnable', 'SetStreamSource', 'SetIndices', 'SetViewport', 'SetScissorRect', 'SetNPatchMode',
  'SetClipPlane']);
const STATE_SETTERS = new Set([...RECORDABLE, 'SetRenderTarget', 'SetDepthStencilSurface']);
function words(line) { return line.split(' '); }
function hexDwords(tok) { return tok.startsWith('[') ? tok.slice(1, -1).split(',') : null; }
class Device {
  constructor() { this.s = new Map(); this.rec = null; this.blocks = new Map(); }
  set(k, v) { this.s.set(k, v); }
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
        const start = parseInt(w[1], 16), d = hexDwords(w[2]);
        if (!d) { const cnt = parseInt(w[3], 16); for (let r = 0; r < cnt; r++) this.set(`${kind} ${start + r}`, w[2]); break; }
        for (let r = 0; r * 4 < d.length; r++) this.set(`${kind} ${start + r}`, d.slice(4 * r, 4 * r + 4).join(','));
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
    const w = words(line); const m = w[0];
    const failed = w.some(t => t.startsWith('=') && t !== '=0');
    if (m === 'BeginStateBlock') { if (!failed && !this.rec) this.rec = []; return false; }
    if (m === 'EndStateBlock') {
      const i = w.indexOf('->');
      if (i > 0 && this.rec) this.blocks.set(w[i + 1], this.rec);
      this.rec = null; return false;
    }
    if (m === 'Capture') { const b = this.blocks.get(w[1]); if (b) for (const r of b) this.capture(r); return false; }
    if (m === 'Apply') { const b = this.blocks.get(w[1]); if (b) for (const r of b) this.apply(r); return false; }
    if (this.rec && RECORDABLE.has(m)) { this.rec.push(w.slice()); return false; }
    if (STATE_SETTERS.has(m)) { this.apply(w); return false; }
    return m.startsWith('Draw');
  }
  // com.c's Capture: the current value into a recorded call of these kinds
  capture(r) {
    const g = k => this.s.get(k);
    switch (r[0]) {
      case 'SetRenderState': if (g(`rs ${r[1]}`) !== undefined) r[2] = g(`rs ${r[1]}`); break;
      case 'SetTextureStageState': if (parseInt(r[1], 16) < 8 && g(`tss ${r[1]} ${r[2]}`) !== undefined) r[3] = g(`tss ${r[1]} ${r[2]}`); break;
      case 'SetSamplerState': if (parseInt(r[1], 16) < 16 && g(`ss ${r[1]} ${r[2]}`) !== undefined) r[3] = g(`ss ${r[1]} ${r[2]}`); break;
      case 'SetTexture': if (parseInt(r[1], 16) < 8 && g(`tex ${r[1]}`) !== undefined) r[2] = g(`tex ${r[1]}`); break;
      case 'SetVertexShader': if (g('vs') !== undefined) r[1] = g('vs'); break;
      case 'SetPixelShader': if (g('ps') !== undefined) r[1] = g('ps'); break;
      case 'SetVertexDeclaration': if (g('decl') !== undefined) r[1] = g('decl'); break;
      case 'SetFVF': if (g('fvf') !== undefined) r[1] = g('fvf'); break;
      default: break;
    }
  }
  snapshot() { return new Map(this.s); }
}
function stateDiff(a, b) {
  const out = [];
  for (const [k, v] of a) if (b.get(k) !== v) out.push(`${k}: ${v} | ${b.get(k) ?? '-'}`);
  for (const [k, v] of b) if (!a.has(k)) out.push(`${k}: - | ${v}`);
  return out;
}

// ---- the run ----
const A = read(files[0]), B = read(files[1]);
const devA = new Device(), devB = new Device();
const nums = [...new Set([...A.keys(), ...B.keys()])].sort((x, y) => x - y);
let callsA = 0, callsB = 0, callDiff = 0, framesDiff = 0, onlyOne = 0, drawsA = 0, drawsB = 0, drawDiff = 0;
const shownCalls = [], shownDraws = [];
const isCall = s => !/^E(-|\d)/.test(s);   // "E<id> ..." and "E- ..." are context lines
for (const no of nums) {
  const fa = A.get(no) ?? [], fb = B.get(no) ?? [];
  const inRange = no >= opt.lo && no <= opt.hi;
  // replay every frame (state carries over), compare the frames in range
  const da = [], db = [];
  for (const l of fa) if (isCall(l) && devA.call(l)) da.push({ l, s: devA.snapshot() });
  for (const l of fb) if (isCall(l) && devB.call(l)) db.push({ l, s: devB.snapshot() });
  if (!inRange) continue;
  if (!A.has(no) || !B.has(no)) { onlyOne++; continue; }
  const ca = fa.filter(isCall), cb = fb.filter(isCall);
  callsA += ca.length; callsB += cb.length; drawsA += da.length; drawsB += db.length;
  const ops = diff(ca, cb);
  if (ops.length) {
    framesDiff++; callDiff += ops.length;
    if (shownCalls.length < opt.show) shownCalls.push({ no, ops: ops.slice(0, 12), n: ops.length });
  }
  for (let i = 0; i < Math.max(da.length, db.length); i++) {
    const x = da[i], y = db[i];
    const d = x && y ? stateDiff(x.s, y.s) : ['(a draw only one side made)'];
    if (d.length || (x && y && x.l !== y.l)) {
      drawDiff++;
      if (shownDraws.length < opt.show) shownDraws.push({ no, i, a: x?.l, b: y?.l, d: d.slice(0, 10), n: d.length });
    }
  }
}
console.log(`A ${files[0]}\nB ${files[1]}`);
console.log(`frames: ${nums.length} (${onlyOne} in one file only); calls A ${callsA}, B ${callsB}; draws A ${drawsA}, B ${drawsB}`);
console.log(`calls: ${callDiff} differences in ${framesDiff} frames`);
console.log(`draws: ${drawDiff} draws whose state differs`);
if (!opt.quiet) {
  for (const s of shownCalls) {
    console.log(`\n-- frame ${s.no}: ${s.n} call differences, the first ones (- A only, + B only):`);
    for (const o of s.ops) console.log(`  ${o.op} ${o.s.slice(0, 160)}`);
  }
  for (const s of shownDraws) {
    console.log(`\n-- frame ${s.no} draw ${s.i}: A ${s.a ?? '-'} | B ${s.b ?? '-'}; ${s.n} states differ (A | B):`);
    for (const d of s.d) console.log(`  ${d.slice(0, 200)}`);
  }
}
console.log(`result: ${callDiff} call differences, ${drawDiff} draw differences`);
process.exit(callDiff || drawDiff ? 1 : 0);
