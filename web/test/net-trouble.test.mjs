// What the page tells the player and its log when the way to the switch is the trouble (web/src/net/index.ts check,
// trouble, the quiet links):
//   node --test web/test/net-trouble.test.mjs
// - connections that never open (a browser says only 1006 of them): after CHECK_AFTER in a row the page asks the
//   server's GET /lan?check once; "busy" (too many from this network's address, where the server's upgrade answers
//   429) and "ok" with the next connection failing too (something on the way stops it) are said to the player once a
//   game, and again when the LAN or Online screens open while the switch is still not there; "ok" with the next one in
//   (a server back from a restart) is not
// - links that go quiet (welcomed, pinging, no answer to any ping, gone), as a network that stops carrying them after
//   a few KB: told once after QUIET_LINKS in a row; links whose pings are answered are not
// The page's net code (bundled as the app bundles it) with a fake game process, against small servers of this file's.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { WebSocket as WsClient, WebSocketServer } from 'ws';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export { attachNet } from './web/src/net/index.ts'; export * as P from './web/src/net/proto.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { attachNet, P } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

// node's own WebSocket reports a refused handshake with an error and no close; a browser's (and ws's) closes it too
globalThis.WebSocket = WsClient;
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const until = async (f, what, ms = 8000) => { const t0 = Date.now(); while (!f()) { if (Date.now() - t0 > ms) throw new Error(`${what}: timed out`); await sleep(10); } };

function mailbox(memory, ptr) {
  const hdr = P.NET_HDR_BYTES, w = new Int32Array(memory.buffer, ptr, hdr / 4);
  w[P.W_MAGIC] = P.NET_MAGIC; w[P.W_VERSION] = P.NET_VERSION; w[P.W_HDR_BYTES] = hdr; w[P.W_SLOT_BYTES] = P.NET_SLOT_BYTES;
  w[P.W_TX_SLOTS] = P.NET_TX_SLOTS; w[P.W_RX_SLOTS] = P.NET_RX_SLOTS;
  w[P.W_TX_OFF] = hdr; w[P.W_RX_OFF] = hdr + P.NET_TX_SLOTS * P.NET_SLOT_BYTES;
  return { w, ptr, size: P.NET_MAILBOX_BYTES };
}
function want(mb, on) { Atomics.store(mb.w, P.W_WANT, on); Atomics.add(mb.w, P.W_TX_BELL, 1); Atomics.notify(mb.w, P.W_TX_BELL); }
/** a game whose free-roam mailbox wants the switch from the start, and whose LAN play mailbox opens when told */
function game(port, opts = {}) {
  const pages = Math.ceil(2 * P.NET_MAILBOX_BYTES / 65536) + 1;
  const memory = new WebAssembly.Memory({ initial: pages, maximum: pages, shared: true });
  const roam = mailbox(memory, 0), lanMb = mailbox(memory, P.NET_MAILBOX_BYTES);
  want(roam, 1);
  const boxes = { 4: roam, 7: lanMb };
  const proc = { memory, ctl: new Int32Array(1), exited: new Promise(() => {}), registered: k => Promise.resolve({ ptr: boxes[k].ptr, size: boxes[k].size }) };
  const logs = [], notices = [];
  const link = attachNet(proc, { url: `ws://127.0.0.1:${port}/lan`, name: 'P', log: l => logs.push(l), onNotice: t => notices.push(t), ...opts });
  return { link, logs, notices, openLan: () => want(lanMb, 1) };
}

/** a server whose /lan upgrade fails as `refuse` makes it, and whose GET /lan?check says `check` */
async function refusing(refuse, check) {
  const seen = { upgrades: 0, checks: 0 };
  const srv = http.createServer((q, s) => {
    if (q.url === '/lan?check') { seen.checks++; s.writeHead(200, { 'content-type': 'application/json' }); s.end(JSON.stringify({ lan: check })); return; }
    s.writeHead(404); s.end();
  });
  srv.on('upgrade', (q, sock) => { seen.upgrades++; refuse(sock); });
  await new Promise(r => srv.listen(0, '127.0.0.1', r));
  globalThis.location ??= new URL(`http://127.0.0.1:${srv.address().port}/`);
  return { port: srv.address().port, seen, close: () => new Promise(r => { srv.closeAllConnections(); srv.close(r); }) };
}
const answer429 = sock => { sock.end('HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'); };

const BUSY = /^Too many players are connected from your network/;
const BLOCKED = /^Your antivirus or network is probably blocking the connection to other players/;
const QUIET = /^Your connection to other players keeps dropping/;

test('connections the server turns away (429): the page asks once, logs why and tells the player once', async () => {
  const s = await refusing(answer429, 'busy');
  const g = game(s.port);
  try {
    await until(() => g.notices.length === 1, 'the notice');
    assert.match(g.notices[0], BUSY);
    assert.equal(s.seen.checks, 1);
    assert.ok(s.seen.upgrades >= 3, `${s.seen.upgrades} upgrades before the check`);
    assert.ok(g.logs.some(l => /never opened: the server turns them away, too many from this network's address/.test(l)), g.logs.join('\n'));
    assert.ok(g.logs.some(l => /still no switch \(\d+ attempts, the last never open\)/.test(l)), g.logs.join('\n'));
    // it keeps trying, and asks no more in this run of failures
    const n = s.seen.upgrades;
    await until(() => s.seen.upgrades >= n + 2, 'more tries');
    assert.equal(s.seen.checks, 1);
    assert.equal(g.notices.length, 1);
    // the LAN or Online screens open: told again there
    g.openLan();
    await until(() => g.notices.length === 2, 'told again as LAN play opens');
    assert.match(g.notices[1], BUSY);
  } finally { g.link.close(); await s.close(); }
});

test('connections the server would take but that never open: something on the way stops them', async () => {
  const s = await refusing(sock => sock.destroy(), 'ok');
  const g = game(s.port);
  try {
    await until(() => g.notices.length === 1, 'the notice');
    assert.match(g.notices[0], BLOCKED);
    assert.equal(s.seen.checks, 1);
    assert.ok(g.logs.some(l => /never opened, but the server would take one now/.test(l)), g.logs.join('\n'));
    assert.ok(g.logs.some(l => /never opened either, though the server would take it: something on the way stops them/.test(l)), g.logs.join('\n'));
  } finally { g.link.close(); await s.close(); }
});

test('a server back from a restart (its check says "ok", the next connection gets in): nothing said to the player', async () => {
  const srv = http.createServer((q, s) => { s.writeHead(200, { 'content-type': 'application/json' }); s.end('{"lan":"ok"}'); });
  const wss = new WebSocketServer({ noServer: true });
  let upgrades = 0;
  srv.on('upgrade', (q, sock, head) => {
    if (++upgrades <= 3) return sock.destroy();
    wss.handleUpgrade(q, sock, head, ws => ws.send(JSON.stringify({ t: 'welcome', vip: (10 << 24 | 77 << 16 | 1 << 8 | 5) >>> 0, mask: '255.255.0.0', token: 't'.repeat(32) })));
  });
  await new Promise(r => srv.listen(0, '127.0.0.1', r));
  const g = game(srv.address().port);
  try {
    await g.link.online;
    assert.ok(g.logs.some(l => /never opened, but the server would take one now/.test(l)), g.logs.join('\n'));
    await sleep(300);
    assert.deepEqual(g.notices, []);
    assert.ok(!g.logs.some(l => /something on the way/.test(l)));
  } finally { g.link.close(); wss.close(); srv.closeAllConnections(); await new Promise(r => srv.close(r)); }
});

test('a server of before (no check) and one that says "fast": logged, nothing said to the player', async () => {
  for (const [check, line] of [[undefined, /never opened; the server's check: 200/], ['fast', /they came too fast/]]) {
    const s = await refusing(answer429, check);
    const g = game(s.port);
    try {
      await until(() => s.seen.checks === 1 && g.logs.some(l => line.test(l)), `the log for ${check}`);
      await sleep(200);
      assert.deepEqual(g.notices, []);
    } finally { g.link.close(); await s.close(); }
  }
});

/** a switch that welcomes each connection and, unless `answers`, never answers its pings and lets it go after `ms`
 *  (as a link the network stopped carrying, which the switch's keepalive cuts) */
async function switchOf({ answers, ms }) {
  const srv = http.createServer((q, s) => { s.writeHead(404); s.end(); });
  const wss = new WebSocketServer({ noServer: true });
  let n = 0;
  srv.on('upgrade', (q, sock, head) => wss.handleUpgrade(q, sock, head, ws => {
    n++;
    ws.send(JSON.stringify({ t: 'welcome', vip: (10 << 24 | 77 << 16 | 1 << 8 | 5) >>> 0, mask: '255.255.0.0', token: 't'.repeat(32), bundle: 1 }));
    ws.on('message', d => {
      const m = JSON.parse(String(d));
      if (answers && m.t === 'ping') ws.send(JSON.stringify({ t: 'pong', id: m.id }));
    });
    setTimeout(() => ws.terminate(), ms);
  }));
  await new Promise(r => srv.listen(0, '127.0.0.1', r));
  return { port: srv.address().port, links: () => n, close: () => new Promise(r => { wss.close(); srv.closeAllConnections(); srv.close(r); }) };
}

test('links that go quiet, three in a row: told once; links whose pings are answered: not', async () => {
  const quiet = await switchOf({ answers: false, ms: 200 });
  const g = game(quiet.port, { pingMs: 40 });
  try {
    await until(() => g.notices.length === 1, 'the notice');
    assert.match(g.notices[0], QUIET);
    assert.ok(quiet.links() >= 3, `${quiet.links()} links`);
    assert.ok(g.logs.some(l => /the last 3 links to the switch went quiet/.test(l)), g.logs.join('\n'));
    const n = quiet.links();
    await until(() => quiet.links() >= n + 3, 'three more');
    assert.equal(g.notices.length, 1, 'once a game');
  } finally { g.link.close(); await quiet.close(); }
  const fine = await switchOf({ answers: true, ms: 200 });
  const h = game(fine.port, { pingMs: 40 });
  try {
    await until(() => fine.links() >= 5, 'links that drop but answered');
    assert.deepEqual(h.notices, []);
    assert.ok(!h.logs.some(l => /went quiet/.test(l)));
  } finally { h.link.close(); await fine.close(); }
});
