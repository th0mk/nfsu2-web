// The recorded horns (runtime/horn.c's kinds with a clip): cut from their recordings, as web/src/static/horns.dat,
// which the game embeds (horn.c) and the start page fetches for Settings' Listen button (horn-preview.ts).
//   node web/tools/mkhorns.mjs <folder> [out]
// <folder> holds the recordings as <YouTube id>.<any extension ffmpeg reads>, e.g. from
//   yt-dlp -x --audio-format wav -o '%(id)s.%(ext)s' -- <id> ...
// Needs ffmpeg.  Each clip: cut, one channel at RATE, the silence before and after it trimmed, a fade in (and out,
// but for a looping clip), its peak at -1 dBFS (horn.c's level per kind sets how loud it sounds).  soft: its spikes
// rounded off (c tanh(x / c), c that much of its peak), so it sounds as loud as the others with no higher peaks.  A looping clip
// (loop: seconds after its start) ends at the loop's end, which is cross-faded with what comes before the loop's
// start, so the jump back is seamless.
//
// horns.dat, little-endian: "HRN1", u16 count, u16 rate; count entries of { char name[16] (NUL-padded), u32 offset,
// u32 samples, u32 loop start, u32 loop end (0, 0: none) } (offsets and points in samples, from the data's start);
// then the data, int16.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const RATE = 32000;
/** the clips, by the name horn.c's kinds give; from/to: seconds in the recording */
const CLIPS = [
  { name: 'dixie', src: '-wllulJyJa8', from: 1.0, to: 4.2 },   // "Goofy ahh car honk sound effect": a Dixie horn
  { name: 'oldcar', src: 'wrmQ2KHHQPc', from: 0.3, to: 2.0 },  // "Old Car Horn - Sound Effect(HD)"
  { name: 'broken', src: 'RtQN0sRRbvM', from: 0.5, to: 1.7 },  // "1990 Manual Toyota Corolla Broken Horn Honks": its first, from the honk
  { name: 'thecar', src: 'ds3r2nT7YeU', from: 1.8, to: 4.6 },  // "The car (1977) horn SFX": a short and a long honk
  { name: 'thecar_long', src: 'ds3r2nT7YeU', from: 13.5, to: 16.6, loop: [0.5, 2.5], soft: 0.45 },   // the same, its held blast
];
const XFADE_S = 0.15, FADE_IN_S = 0.005, FADE_OUT_S = 0.03, GATE_DB = -40, PEAK_DB = -1;

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const [dir, out = path.join(REPO, 'web/src/static/horns.dat')] = process.argv.slice(2);
if (!dir) { console.error('usage: node web/tools/mkhorns.mjs <folder of recordings> [out]'); process.exit(2); }

function clip(c) {
  const file = fs.readdirSync(dir).find(f => path.parse(f).name === c.src);
  if (!file) throw new Error(`${c.name}: no ${c.src}.* in ${dir}`);
  const raw = execFileSync('ffmpeg', ['-v', 'error', '-ss', String(c.from), '-to', String(c.to), '-i', path.join(dir, file),
    '-ac', '1', '-ar', String(RATE), '-f', 'f32le', '-'], { maxBuffer: 1 << 28 });
  let x = new Float32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.length));
  let peak = x.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
  const gate = peak * 10 ** (GATE_DB / 20);
  let a = x.findIndex(v => Math.abs(v) > gate), b = x.length;
  if (!c.loop) while (b > a && Math.abs(x[b - 1]) <= gate) b--;
  a = Math.max(0, a - Math.round(0.002 * RATE));
  x = x.slice(a, b);
  let l0 = 0, l1 = 0;
  if (c.loop) {
    [l0, l1] = c.loop.map(s => Math.round(s * RATE));
    const n = Math.round(XFADE_S * RATE);
    if (l0 < n || l1 > x.length || l1 - l0 < 2 * n) throw new Error(`${c.name}: the loop does not fit`);
    for (let i = 0; i < n; i++) {   // equal power: the end turns into what led to the start
      const w = (i + 0.5) / n;
      x[l1 - n + i] = x[l1 - n + i] * Math.cos(w * Math.PI / 2) + x[l0 - n + i] * Math.sin(w * Math.PI / 2);
    }
    x = x.slice(0, l1);
  }
  if (c.soft) for (let i = 0; i < x.length; i++) x[i] = c.soft * peak * Math.tanh(x[i] / (c.soft * peak));
  const fi = Math.round(FADE_IN_S * RATE), fo = c.loop ? 0 : Math.round(FADE_OUT_S * RATE);
  for (let i = 0; i < fi; i++) x[i] *= i / fi;
  for (let i = 0; i < fo; i++) x[x.length - 1 - i] *= i / fo;
  peak = x.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
  const g = 10 ** (PEAK_DB / 20) / peak;
  const pcm = Int16Array.from(x, v => Math.max(-32768, Math.min(32767, Math.round(v * g * 32767))));
  console.log(`${c.name.padEnd(12)} ${(pcm.length / RATE).toFixed(2)} s${c.loop ? `, loop ${(l0 / RATE).toFixed(2)}-${(l1 / RATE).toFixed(2)} s` : ''}`);
  return { name: c.name, pcm, l0, l1 };
}

const clips = CLIPS.map(clip), HEAD = 8 + 32 * clips.length;
const total = clips.reduce((n, c) => n + c.pcm.length, 0), buf = Buffer.alloc(HEAD + 2 * total);
buf.write('HRN1', 0, 'latin1'); buf.writeUInt16LE(clips.length, 4); buf.writeUInt16LE(RATE, 6);
let off = 0;
clips.forEach((c, i) => {
  const e = 8 + 32 * i;
  buf.write(c.name, e, 16, 'latin1');
  buf.writeUInt32LE(off, e + 16); buf.writeUInt32LE(c.pcm.length, e + 20); buf.writeUInt32LE(c.l0, e + 24); buf.writeUInt32LE(c.l1, e + 28);
  Buffer.from(c.pcm.buffer, c.pcm.byteOffset, c.pcm.byteLength).copy(buf, HEAD + 2 * off);
  off += c.pcm.length;
});
fs.writeFileSync(out, buf);
console.log(`${out}: ${clips.length} clips, ${(total / RATE).toFixed(1)} s, ${buf.length} bytes`);
