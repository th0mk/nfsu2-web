// Host self-test page: runs a test module (?wasm=...) through the real host
// (pool, I/O worker, mounts) and reports its output; title becomes DONE.
import { startProcess } from '../host/process';
import { attachNet } from '../net/index';
import { attachNetWorker } from '../net/remote';
import { mainBurn } from './burn';
const q = new URLSearchParams(location.search);
const pre = document.createElement('pre'); document.body.append(pre);
const log = (s: string) => { pre.append(s + '\n'); console.log(s); };
const t0 = performance.now();
const mod = await WebAssembly.compileStreaming(fetch(q.get('wasm') ?? '/test/hostcheck.wasm'));
log(`compiled in ${(performance.now() - t0).toFixed(0)} ms`);
if (q.has('fresh')) {
  const root = await navigator.storage.getDirectory();
  for await (const [name] of (root as any).entries()) if (name === (q.get('save') ?? 'save-test')) await root.removeEntry(name, { recursive: true });
}
// ?reg=/tests/web/seed/x.txt: registry seed, copied in as /save/registry.txt
async function preload(url: string, name: string) {
  const txt = await (await fetch(url, { cache: 'no-store' })).text();
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(q.get('save') ?? 'save-test', { create: true });
  const w = await (await dir.getFileHandle(name, { create: true })).createWritable();
  await w.write(txt); await w.close();
}
const env0: Record<string, string> = {};
if (q.get('reg')) { await preload(q.get('reg')!, 'registry.txt'); env0.NFSU2_REGFILE = '/save/registry.txt'; }
// ?script=/tests/scripts/x.txt: an autotest input script, copied into the save dir
const env: Record<string, string> = env0;
const script = q.get('script');
if (script) {
  const txt = await (await fetch(script, { cache: 'no-store' })).text();
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle(q.get('save') ?? 'save-test', { create: true });
  const w = await (await dir.getFileHandle('autotest-script.txt', { create: true })).createWritable();
  await w.write(txt); await w.close();
  env.NFSU2_INPUT_SCRIPT = '/save/autotest-script.txt';
}
const allEnv: Record<string, string> = { HOME: '/save', NFSU2_DATA: '/data', ...env, ...Object.fromEntries(q.getAll('env').map(e => [e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1)])) };
const p = await startProcess({
  module: mod, pool: +(q.get('pool') ?? 24), initialPages: +(q.get('pages') ?? 1024),
  cfg: { args: ['nfsu2', ...(q.getAll('arg'))], env: allEnv, dataBase: '/data/', manifestUrl: '/data-manifest', saveDir: q.get('save') ?? 'save-test' },
  onLog: (s, fd, tid) => log(`[${tid}${fd === 2 ? '!' : ''}] ${s}`),
  onNote: log,
});
// ?net=<name>: the virtual LAN (web/src/net) under that player name; the notices the app shows over the game, logged.
// Free roam's friends (app/friends.ts), for tests: NFSU2_TEST_CLAIM is the keyphrase token the hello claims with,
// front bumpers and the server's friend messages are logged ([bump], [friend]), the touched player is asked about at
// once, and with NFSU2_TEST_FRIEND=add the page asks to befriend it (or accepts) as soon as the server says it may
const autoAdd = allEnv.NFSU2_TEST_FRIEND === 'add';
// ?networker=0: the link on the page's main thread (attachNet) instead of its worker (attachNetWorker, as the app)
const net = q.has('net') ? (q.get('networker') === '0' ? attachNet : attachNetWorker)(p, {
  name: q.get('net') || 'test', log, claim: allEnv.NFSU2_TEST_CLAIM || undefined,
  onNotice: t => log(`[notice] ${t}`), onLobby: j => log(`[lobby] ${j}`),
  onBump: j => {
    log(`[bump] ${j}`);
    const m = JSON.parse(j);
    if (m.on === 1) net?.friend({ op: 'can', name: m.name });
  },
  onFriend: m => {
    log(`[friend] ${JSON.stringify(m)}`);
    if (autoAdd && ((m.op === 'can' && (m.st === 'new' || m.st === 'asks')) || m.op === 'asks')) net?.friend({ op: 'add', name: m.name });
  },
}) : null;
void net?.online.then(() => { net.friend({ op: 'list' }); });
(globalThis as any).nfsu2Net = net;
mainBurn(q, log);
const code = await p.exited;
if (net) log(`NET ${JSON.stringify(net.stats())}`);
log(`EXIT ${code}`); document.title = 'DONE';
