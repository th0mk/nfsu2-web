// A slow line in front of a local server, for tests of what a player on a slow connection waits for (the game's
// data blocks, the module, a friend's car with an empty cache): every HTTP answer's bytes through one shared
// bucket of --mbit Mbit/s, each request --rtt ms late (half on the way there, half before its answer starts);
// WebSockets (/lan) through as they are, --ws-rtt ms each way if given.
//
//   node web/tools/slow-line.mjs --to http://127.0.0.1:8611 [--port 8612] [--mbit 20] [--rtt 60] [--ws-rtt 0]
//        [--fast /game.wasm,/game.json]
//   --fast   paths (prefixes) answered at full speed: the module, so that a test waits only for what it measures
// Every 10 s while it carries anything: requests, MB through, the bucket's wait (how far behind the line is).
import http from 'node:http';
import net from 'node:net';

const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const TO = new URL(opt('to', 'http://127.0.0.1:8611')), PORT = +opt('port', 8612), MBIT = +opt('mbit', 20);
const RTT = +opt('rtt', 60), WS_RTT = +opt('ws-rtt', 0), FAST = opt('fast', '/game.wasm,/game.json').split(',').filter(Boolean);
const rate = MBIT * 1e6 / 8;                        // bytes a second
let free = performance.now();                       // when the line is next free (ms)
const st = { req: 0, bytes: 0, waitMs: 0 };
const byPath = new Map();                           // the bytes through the line by file (data blocks: their file)
const sleep = ms => new Promise(r => setTimeout(r, ms));

/** a chunk through the line: it leaves when the line has carried every byte before it */
async function line(n) {
  const now = performance.now();
  free = Math.max(free, now) + n / rate * 1000;
  st.bytes += n;
  const wait = free - now;
  st.waitMs = Math.max(st.waitMs, wait);
  if (wait > 1) await sleep(wait);
}

const server = http.createServer(async (req, res) => {
  st.req++;
  const fast = FAST.some(p => req.url.startsWith(p));
  // (the request's body, if any, held while it is on its way)
  req.pause();
  if (!fast && RTT) await sleep(RTT / 2);
  const up = http.request({ host: TO.hostname, port: TO.port, method: req.method, path: req.url, headers: req.headers }, async r => {
    if (!fast && RTT) await sleep(RTT / 2);
    res.writeHead(r.statusCode, r.statusMessage, r.headers);
    if (fast) { r.pipe(res); return; }
    // each chunk after the one before (an answer already in can end before its chunks are through the line)
    let chain = Promise.resolve();
    const key = decodeURIComponent(req.url.replace(/^\/data(-blocks)?\/(v\d+\/)?/, '').replace(/\?.*$/, ''));
    r.on('data', c => {
      byPath.set(key, (byPath.get(key) ?? 0) + c.length);
      r.pause();
      chain = chain.then(() => line(c.length)).then(() => { res.write(c); r.resume(); });
    });
    r.on('end', () => { chain = chain.then(() => res.end()); });
    r.on('error', () => res.destroy());
  });
  up.on('error', e => { res.writeHead(502); res.end(String(e)); });
  req.pipe(up);
});
// WebSockets: the upgrade request and everything after it, both ways (a delay each way with --ws-rtt)
server.on('upgrade', (req, sock, head) => {
  const up = net.connect(+TO.port, TO.hostname, () => {
    let h = `${req.method} ${req.url} HTTP/1.1\r\n`;
    for (let i = 0; i < req.rawHeaders.length; i += 2) h += `${req.rawHeaders[i]}: ${req.rawHeaders[i + 1]}\r\n`;
    up.write(h + '\r\n'); if (head?.length) up.write(head);
    const pipe = (a, b) => WS_RTT
      ? a.on('data', c => setTimeout(() => b.write(c), WS_RTT / 2))
      : a.pipe(b);
    pipe(up, sock); pipe(sock, up);
  });
  const end = () => { up.destroy(); sock.destroy(); };
  up.on('error', end); sock.on('error', end); up.on('close', end); sock.on('close', end);
});
server.listen(PORT, '127.0.0.1', () => console.log(`slow-line: http://127.0.0.1:${PORT} -> ${TO.origin}, ${MBIT} Mbit/s, ${RTT} ms`));
let last = { ...st };
setInterval(() => {
  if (st.req === last.req && st.bytes === last.bytes) return;
  console.log(`slow-line: ${st.req - last.req} requests, ${((st.bytes - last.bytes) / 1e6).toFixed(1)} MB in 10 s, the line up to ${(st.waitMs / 1000).toFixed(1)} s behind`);
  // (by kind: a car's files as CARS/*/<file>, the rest by their first two path parts; the 10 largest so far)
  const kinds = new Map();
  for (const [k, v] of byPath) {
    const d = /^CARS\/[^/]+\/[^/]+$/i.test(k) ? k.replace(/^CARS\/[^/]+\//i, 'CARS/*/') : k.split('/').slice(0, 2).join('/');
    kinds.set(d, (kinds.get(d) ?? 0) + v);
  }
  console.log(`slow-line:   so far by kind: ${[...kinds].sort((a, b) => b[1] - a[1]).slice(0, 10).map(([d, v]) => `${d} ${(v / 1e6).toFixed(1)} MB`).join(', ')}`);
  last = { ...st }; st.waitMs = 0;
}, 10000).unref?.();
