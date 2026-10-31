// What a device-call recording (NFSU2_DXREC, docs/web/dxrec.md) holds for Most Wanted's effects: per window of
// Presents (the input script's "dxrec N" windows, tests/mw/scripts/fx_census.txt), per Present on average:
//   - draws by effect, technique and pass (the effect context lines E<id> BeginPass .. EndPass; the technique is the
//     index of the effect's last GetTechnique before its SetTechnique, "-" when the game set it before the window),
//     and draws outside any effect;
//   - the render targets drawn into, in order, with their viewport sizes and draws;
//   - StretchRect copies, clears, occlusion queries (CreateQuery, Issue, GetData and its answers) and the readbacks
//     (GetRenderTargetData, GetFrontBufferData, UpdateSurface, ColorFill);
//   node web/tools/fx-census.mjs <dxrec.txt> [--vs <other dxrec.txt> [--calls [N]]] [--json] [--targets]
// --vs puts a second recording beside the first, window by window (an effect on and off: what only one side draws),
// with each key's draws a Present on both sides.  Its numbers compare like for like only where both runs were at the
// same place and moment: fx_census.txt's GO window (every run stands on the start line); the later windows wait for
// a frame count and a speed, which two runs reach at different places, so their differences are the drive's as much
// as an effect's.  --vs --calls compares the device calls themselves, without pairing frames (dxrec-diff pairs only
// frames whose draws are all the same, which two runs' windows seldom are): per effect pass, the distinct state
// calls made inside it (Set*, state blocks' Apply and Capture; objects renumbered per file in the order it first
// names them), and lists the calls only one side made, N of them per key (default 4).  Run it on two runs of one
// mode as well (HLE and HLE, oracle and oracle): their differences are the noise.
// --json prints the windows as JSON; --targets lists each Present's target sequence too.
import { readFileSync } from 'node:fs';

const FX = ['WORLD', 'WORLDREFLECT', 'WORLDBONE', 'WORLDNORMALMAP', 'CAR', 'GLOSSYWINDOW', 'TREE', 'WORLDMIN',
  'WORLDNOFOG', 'FE', 'FE_MASK', 'FILTER', 'OVERBRIGHT', 'SCREENFILTER', 'RAIN_DROP', 'RUNWAYLIGHT',
  'VISUALTREATMENT', 'WORLDPRELIT', 'PARTICLES', 'SKYBOX', 'SHADOW_MAP_MESH', 'SKYBOX_CG', 'SHADOW_CG',
  'CAR_SHADOW_MAP', 'WORLDDEPTH', 'WORLDNORMALMAPDEPTH', 'CARDEPTH', 'GLOSSYWINDOWDEPTH', 'TREEDEPTH',
  'SHADOW_MAP_MESH_DEPTH', 'WORLDNORMALMAPNOFOG'];
const DRAWS = new Set(['DrawPrimitive', 'DrawIndexedPrimitive', 'DrawPrimitiveUP', 'DrawIndexedPrimitiveUP']);
const READBACKS = new Set(['GetRenderTargetData', 'GetFrontBufferData', 'UpdateSurface', 'ColorFill']);
// "[a,b,c,d,...]" -> hex words
const words = s => (/\[([^\]]*)\]/.exec(s)?.[1] ?? '').split(',').filter(Boolean);
const fxName = k => {
  if (k === 'none') return 'no effect';
  const [id, t, p] = k.split(':');
  return `${id === '-' ? '?' : (FX[+id] ?? id)} t${t} p${p}`;
};

export function census(file, withSequence = false, withCalls = false) {
  const windows = [];
  // --calls: objects renumbered per kind in the order the file first names them (as dxrec-diff --renumber)
  const seen = new Map(), next = new Map();
  const renum = l => l.indexOf('#') < 0 ? l : l.replace(/\b([a-z]+)#(\d+)\b/g, (m, k, n) => {
    const key = `${k}#${n}`;
    if (!seen.has(key)) { const c = (next.get(k) ?? 0) + 1; next.set(k, c); seen.set(key, `${k}#${c}`); }
    return seen.get(key);
  });
  let win = null, frame = null, lastF = -2;
  const tech = {}, lastGet = {};  // effect id -> technique index; its last GetTechnique
  let inPass = null;              // "id:t:p" while an effect pass is open
  let target = 'screen', vp = '';
  const newFrame = n => {
    if (!win || n !== lastF + 1) { win = { from: n, to: n, frames: [] }; windows.push(win); }
    win.to = n; lastF = n;
    frame = { n, draws: 0, byFx: {}, targets: [{ t: target, vp, draws: 0 }], stretch: [], clears: 0,
      queries: { create: [], issue: {}, getdata: {}, visible: [] }, readbacks: {}, ps: new Set(), calls: withCalls ? {} : null };
    win.frames.push(frame);
  };
  for (const ln of readFileSync(file, 'utf8').split('\n')) {
    if (!ln) continue;
    if (ln.startsWith('F ')) { newFrame(+ln.slice(2)); continue; }
    if (!frame) continue;
    const sp = ln.indexOf(' '), m = sp < 0 ? ln : ln.slice(0, sp), rest = sp < 0 ? '' : ln.slice(sp + 1);
    if (/^E(\d+|-)$/.test(m)) {
      const id = m.slice(1), [meth, arg] = rest.split(' ');
      if (meth === 'GetTechnique') lastGet[id] = arg;
      else if (meth === 'SetTechnique') tech[id] = lastGet[id] ?? '-';
      else if (meth === 'BeginPass') inPass = `${id}:${tech[id] ?? '-'}:${arg}`;
      else if (meth === 'EndPass' || meth === 'End') inPass = null;
      continue;
    }
    if (frame.calls && (m.startsWith('Set') || m === 'Apply' || m === 'Capture')) {
      const k = inPass ?? 'none', c = frame.calls[k] ??= new Map(), l = renum(ln);
      c.set(l, (c.get(l) ?? 0) + 1);
    }
    if (DRAWS.has(m)) {
      frame.draws++;
      const k = inPass ?? 'none';
      frame.byFx[k] = (frame.byFx[k] ?? 0) + 1;
      frame.targets[frame.targets.length - 1].draws++;
    } else if (m === 'SetRenderTarget') {
      const [idx, s] = rest.split(' ');
      if (idx === '0') { target = s; frame.targets.push({ t: s, vp: '', draws: 0 }); }
    } else if (m === 'SetViewport') {
      const w = words(rest); vp = w.length >= 4 ? `${parseInt(w[2], 16)}x${parseInt(w[3], 16)}` : '?';
      const cur = frame.targets[frame.targets.length - 1];
      if (!cur.vp || cur.draws === 0) cur.vp = vp;
    } else if (m === 'StretchRect') {
      const p = rest.split(' ');
      frame.stretch.push(`${p[0]}->${p.find((x, i) => i > 0 && /^(surf|tex)/.test(x)) ?? p[2]}`);
    } else if (m === 'Clear') frame.clears++;
    else if (m === 'SetPixelShader') frame.ps.add(rest);
    else if (m === 'CreateQuery') frame.queries.create.push(rest);
    else if (m === 'Issue') { const fl = rest.split(' ')[1]; frame.queries.issue[fl] = (frame.queries.issue[fl] ?? 0) + 1; }
    else if (m === 'GetData') {
      const hr = /=([0-9a-f]+)$/.exec(rest)?.[1] ?? '0';
      frame.queries.getdata[hr] = (frame.queries.getdata[hr] ?? 0) + 1;
      const v = / -> (\d+)/.exec(rest); if (v) frame.queries.visible.push(+v[1]);
    } else if (READBACKS.has(m)) frame.readbacks[m] = (frame.readbacks[m] ?? 0) + 1;
  }
  // a window the script opened ("dxrec N") starts with the rest of the Present the command ran in: no draws, left out
  for (const w of windows) if (w.frames.length > 1 && w.frames[0].draws === 0) { w.frames.shift(); w.from++; }
  return windows.filter(w => w.frames.some(f => f.draws > 0)).map(w => {
    const n = w.frames.length, avg = v => +(v / n).toFixed(1);
    const byFx = {}, rb = {}, iss = {}, gd = {}, stretch = {};
    let draws = 0, clears = 0, vis = [], create = [];
    const ps = new Set(), calls = {};
    for (const f of w.frames) {
      if (f.calls) for (const [k, c] of Object.entries(f.calls)) {
        const t = calls[fxName(k)] ??= new Map();
        for (const [l, v] of c) t.set(l, (t.get(l) ?? 0) + v);
      }
      draws += f.draws; clears += f.clears;
      for (const [k, v] of Object.entries(f.byFx)) byFx[k] = (byFx[k] ?? 0) + v;
      for (const [k, v] of Object.entries(f.readbacks)) rb[k] = (rb[k] ?? 0) + v;
      for (const [k, v] of Object.entries(f.queries.issue)) iss[k] = (iss[k] ?? 0) + v;
      for (const [k, v] of Object.entries(f.queries.getdata)) gd[k] = (gd[k] ?? 0) + v;
      for (const s of f.stretch) stretch[s] = (stretch[s] ?? 0) + 1;
      vis = vis.concat(f.queries.visible); create = create.concat(f.queries.create);
      for (const s of f.ps) ps.add(s);
    }
    const fx = Object.fromEntries(Object.entries(byFx).sort((a, b) => b[1] - a[1]).map(([k, v]) => [fxName(k), avg(v)]));
    const last = w.frames[w.frames.length - 1].targets.filter(t => t.draws > 0);
    return { presents: `${w.from}..${w.to}`, n, draws: avg(draws), clears: avg(clears), pixelShaders: ps.size, fx,
      passes: last.length, targets: last.map(t => `${t.t}${t.vp ? ' ' + t.vp : ''}: ${t.draws}`),
      stretch: Object.fromEntries(Object.entries(stretch).map(([k, v]) => [k, avg(v)])),
      queries: { create, issue: iss, getdata: gd,
        visible: vis.length ? { n: vis.length, nonzero: vis.filter(v => v > 0).length, max: Math.max(...vis) } : null },
      readbacks: rb,
      ...(withCalls ? { calls } : {}),
      ...(withSequence ? { sequence: w.frames.map(f => f.targets.filter(t => t.draws > 0).map(t => `${t.t}:${t.draws}`).join(' ')) } : {}) };
  });
}

function print(w) {
  console.log(`window Presents ${w.presents} (${w.n}): ${w.draws} draws, ${w.passes} target runs, ${w.clears} clears, ${w.pixelShaders} pixel shaders a Present`);
  for (const [k, v] of Object.entries(w.fx)) console.log(`  ${k.padEnd(30)} ${v}`);
  console.log(`  targets (last Present): ${w.targets.join(' | ')}`);
  if (Object.keys(w.stretch).length) console.log(`  StretchRect: ${Object.entries(w.stretch).map(([k, v]) => `${k} x${v}`).join(', ')}`);
  const q = w.queries;
  if (q.create.length || Object.keys(q.issue).length || Object.keys(q.getdata).length)
    console.log(`  queries: created ${q.create.length}, Issue ${JSON.stringify(q.issue)}, GetData ${JSON.stringify(q.getdata)}, visible ${JSON.stringify(q.visible)}`);
  if (Object.keys(w.readbacks).length) console.log(`  readbacks: ${JSON.stringify(w.readbacks)}`);
  if (w.sequence) for (const s of w.sequence) console.log(`    ${s}`);
}

function printVs(a, b, i) {
  console.log(`window ${i}: A Presents ${a.presents}, B ${b.presents}: draws ${a.draws} / ${b.draws}, target runs ${a.passes} / ${b.passes}, ` +
    `StretchRect ${Object.values(a.stretch).reduce((s, v) => s + v, 0)} / ${Object.values(b.stretch).reduce((s, v) => s + v, 0)}`);
  const keys = [...new Set([...Object.keys(a.fx), ...Object.keys(b.fx)])].sort();
  const onlyA = keys.filter(k => a.fx[k] && !b.fx[k]), onlyB = keys.filter(k => b.fx[k] && !a.fx[k]);
  console.log(`  only in A: ${onlyA.map(k => `${k} ${a.fx[k]}`).join(', ') || '-'}`);
  console.log(`  only in B: ${onlyB.map(k => `${k} ${b.fx[k]}`).join(', ') || '-'}`);
  const ta = new Set(a.targets.map(t => t.replace(/: \d+$/, ''))), tb = new Set(b.targets.map(t => t.replace(/: \d+$/, '')));
  console.log(`  targets only in A: ${[...ta].filter(t => !tb.has(t)).join(', ') || '-'}; only in B: ${[...tb].filter(t => !ta.has(t)).join(', ') || '-'}`);
  const both = keys.filter(k => a.fx[k] && b.fx[k]);
  if (both.length) console.log(`  draws a Present, A / B: ${both.map(k => `${k} ${a.fx[k]}/${b.fx[k]}`).join(', ')}`);
}

// --calls: per effect pass in both windows, the distinct state calls only one side made
function printCalls(a, b, show) {
  const keys = Object.keys(a.calls).filter(k => b.calls[k]).sort();
  let differ = 0, onlyA = 0, onlyB = 0, distinct = 0;
  const lines = [], byMethod = {};   // method -> [only in A, only in B]
  const tally = (l, side) => { const m = l.slice(0, l.indexOf(' ') >>> 0); (byMethod[m] ??= [0, 0])[side]++; };
  for (const k of keys) {
    const ca = a.calls[k], cb = b.calls[k];
    const xa = [...ca.keys()].filter(l => !cb.has(l)), xb = [...cb.keys()].filter(l => !ca.has(l));
    distinct += new Set([...ca.keys(), ...cb.keys()]).size;
    if (!xa.length && !xb.length) continue;
    differ++; onlyA += xa.length; onlyB += xb.length;
    for (const l of xa) tally(l, 0);
    for (const l of xb) tally(l, 1);
    lines.push(`    ${k}: ${ca.size} / ${cb.size} distinct calls, ${xa.length} only in A, ${xb.length} only in B`);
    for (const l of xa.slice(0, show)) lines.push(`      A: ${l.length > 150 ? l.slice(0, 150) + '...' : l}`);
    for (const l of xb.slice(0, show)) lines.push(`      B: ${l.length > 150 ? l.slice(0, 150) + '...' : l}`);
  }
  console.log(`  calls: ${keys.length} keys in both, ${differ} with calls only one side made; ${distinct} distinct calls, ${onlyA} only in A, ${onlyB} only in B`);
  console.log(`  only one side's, by method (A/B): ${Object.entries(byMethod).sort((x, y) => y[1][0] + y[1][1] - x[1][0] - x[1][1]).map(([m, [x, y]]) => `${m} ${x}/${y}`).join(', ') || '-'}`);
  for (const l of lines) console.log(l);
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const args = process.argv.slice(2);
  const vi = args.indexOf('--vs'), other = vi >= 0 ? args[vi + 1] : null;
  const file = args.find((a, i) => !a.startsWith('--') && (vi < 0 || i !== vi + 1) && !/^\d+$/.test(a));
  if (!file) { console.error('usage: node web/tools/fx-census.mjs <dxrec.txt> [--vs <dxrec.txt> [--calls [N]]] [--json] [--targets]'); process.exit(2); }
  const ci = args.indexOf('--calls'), calls = ci >= 0 && !!other, show = ci >= 0 && /^\d+$/.test(args[ci + 1] ?? '') ? +args[ci + 1] : 4;
  const A = census(file, args.includes('--targets'), calls), B = other ? census(other, args.includes('--targets'), calls) : null;
  if (args.includes('--json')) console.log(JSON.stringify(B ? { a: A, b: B } : A, (k, v) => v instanceof Map ? Object.fromEntries(v) : v));
  else if (B) for (let i = 0; i < Math.max(A.length, B.length); i++) {
    if (A[i] && B[i]) { printVs(A[i], B[i], i); if (calls) printCalls(A[i], B[i], show); }
  }
  else for (const w of A) print(w);
}
