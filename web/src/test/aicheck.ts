// Audio and input test page: runs the game like hostcheck.html (same
// parameters: wasm, pages, pool, arg, env, save, fresh, reg, script) with the
// audio output and the input layer attached to a 1280x720 element.
//   &audio=0          no AudioContext (the pump runs on the wall clock)
//   &input=0          no input layer
//   &suspend=A,B      suspend the AudioContext A s after attaching, for B s,
//                     then resume it
//   &stats=MS         log the page-side audio and input counters every MS ms
//   &quitafter=S      ask the game to quit (WM_CLOSE) S s after start
// The title becomes DONE when the process exits; window.__nfsu2 has the handles.
import { testSaveDir } from '../app/save-dir';
import { startProcess } from '../host/process';
import { attachAudio, type AudioHandle } from '../audio/index';
import { attachInput, type InputHandle } from '../input/index';
import { abs, at } from '../base';
import { DATA_PATH, MANIFEST_PATH } from '../host/datacache';

const q = new URLSearchParams(location.search);
const log = (s: string) => console.log(s);
const t0 = performance.now();
const mod = await WebAssembly.compileStreaming(fetch(q.get('wasm') ?? at('game.wasm')));
log(`compiled in ${(performance.now() - t0).toFixed(0)} ms`);
const saveDir = testSaveDir(q.get('save'), 'test');
if (q.has('fresh')) {
  const root = await navigator.storage.getDirectory();
  for await (const [name] of (root as unknown as { entries(): AsyncIterable<[string, unknown]> }).entries())
    if (name === saveDir) await root.removeEntry(name, { recursive: true });
}
async function preload(url: string, name: string) {
  const txt = await (await fetch(url, { cache: 'no-store' })).text();
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(saveDir, { create: true });
  const w = await (await dir.getFileHandle(name, { create: true })).createWritable();
  await w.write(txt); await w.close();
}
const env: Record<string, string> = {};
if (q.get('reg')) { await preload(q.get('reg')!, 'registry.txt'); env.NFSU2_REGFILE = '/save/registry.txt'; }
if (q.get('script')) { await preload(q.get('script')!, 'autotest-script.txt'); env.NFSU2_INPUT_SCRIPT = '/save/autotest-script.txt'; }

const view = document.createElement('div');
view.style.cssText = 'position:fixed;left:0;top:0;width:1280px;height:720px;background:#000';
document.body.style.margin = '0';
document.body.append(view);

const proc = await startProcess({
  module: mod, pool: +(q.get('pool') ?? 24), initialPages: +(q.get('pages') ?? 16384),
  cfg: {
    args: ['nfsu2', ...q.getAll('arg')],
    env: { HOME: '/save', NFSU2_DATA: '/data', ...env, ...Object.fromEntries(q.getAll('env').map(e => [e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1)])) },
    dataBase: abs(DATA_PATH), manifestUrl: abs(MANIFEST_PATH), saveDir,
  },
  onLog: (s, fd, tid) => log(`[${tid}${fd === 2 ? '!' : ''}] ${s}`),
  onNote: log,
});

let audio: AudioHandle | null = null, input: InputHandle | null = null;
if (q.get('input') !== '0') input = attachInput(proc, view, { log });
if (q.get('audio') !== '0') {
  try {
    audio = await attachAudio(proc, { log });
    log(`[page] AudioContext ${audio.context?.state ?? 'none'} at ${audio.context?.sampleRate ?? 0} Hz`);
  } catch (e) { log(`[page] audio not attached: ${e}`); }
}
(window as unknown as { __nfsu2: unknown }).__nfsu2 = { proc, audio, input };

const sus = q.get('suspend');
if (sus && audio) {
  const [a, b] = sus.split(',').map(Number);
  setTimeout(async () => {
    log(`[page] suspending the AudioContext for ${b} s`);
    await audio!.context?.suspend();
    log(`[page] AudioContext ${audio!.context?.state}`);
    setTimeout(async () => { await audio!.resume(); log(`[page] AudioContext ${audio!.context?.state} again`); }, b * 1000);
  }, a * 1000);
}
const every = +(q.get('stats') ?? 0);
if (every > 0) setInterval(() => {
  if (audio) { const s = audio.status();
    log(`[page] audio ${s.state} mode=${s.mode} fill=${s.fill}/${s.target} underruns=${s.underruns} quanta=${s.quanta} loud=${s.loudQuanta} ` +
        `peak=${s.peakDb.toFixed(1)}dBFS rms=${s.rmsDb.toFixed(1)}dBFS wall=${s.wallMs}ms latency=${s.baseLatencyMs.toFixed(1)}+${s.outputLatencyMs.toFixed(1)}ms`); }
  if (input) { const s = input.status();
    log(`[page] input events=${s.events} dropped=${s.dropped} held=${s.keysHeld} inactive=${s.gameInactive} cursor=${s.cursor} pad=${s.pad ?? '-'}`); }
}, every);
const qa = +(q.get('quitafter') ?? 0);
if (qa > 0 && input) setTimeout(() => { log('[page] quit requested'); input!.requestQuit(); }, qa * 1000);

const code = await proc.exited;
// the game is gone: stop its remaining threads (the audio pump, the input thread ...)
await audio?.close(); input?.detach(); proc.terminate();
log(`EXIT ${code}`); document.title = 'DONE';
