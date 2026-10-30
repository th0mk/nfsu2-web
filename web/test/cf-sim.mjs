// A local stand-in for Cloudflare's proxy in front of the server, for tests
// only (nothing here opens anything to the internet; it listens on 127.0.0.1):
//   browser --TLS--> this proxy --HTTP--> the server --behind-proxy (127.0.0.1)
// It does what the edge does to a request and a response that matter here:
//   - terminates TLS with a test certificate (makeTestCerts)
//   - sets CF-Connecting-IP to the client (replacing whatever the client
//     sent), appends it to X-Forwarded-For, sets X-Forwarded-Proto, CF-Visitor,
//     CF-Ray, CF-IPCountry, and keeps the Host the browser asked for
//   - passes WebSocket upgrades through, and cuts a WebSocket that stays
//     quiet in both directions for wsIdleMs (Cloudflare: about 100 s)
//   - adds a Server: cloudflare header and CF-Ray to responses
// fakeClientIp: the address it reports as the client's (the tests run on one
// machine, where every client is 127.0.0.1).
//
//   node web/test/cf-sim.mjs --port 8527 --target 8627 --dir <cert dir> [--fake-client-ip 198.51.100.7] [--ws-idle-ms 100000]
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const ssl = (args) => execFileSync('openssl', args, { stdio: ['ignore', 'pipe', 'pipe'] });

/**
 * A test CA and a server certificate for localhost, 127.0.0.1 and nfsu2.test,
 * made once in dir.  Returns paths, the PEMs and the SPKI hash Chrome's
 * --ignore-certificate-errors-spki-list takes.
 */
export function makeTestCerts(dir) {
  fs.mkdirSync(dir, { recursive: true });
  const f = n => path.join(dir, n);
  if (!fs.existsSync(f('server.crt'))) {
    ssl(['req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', f('ca.key'), '-out', f('ca.crt'),
      '-days', '30', '-subj', '/CN=NFSU2 deploy test CA', '-addext', 'basicConstraints=critical,CA:TRUE,pathlen:0', '-addext', 'keyUsage=critical,keyCertSign']);
    fs.writeFileSync(f('server.ext'), 'subjectAltName=DNS:localhost,DNS:nfsu2.test,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n');
    ssl(['req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', f('server.key'), '-out', f('server.csr'), '-subj', '/CN=nfsu2.test']);
    ssl(['x509', '-req', '-in', f('server.csr'), '-CA', f('ca.crt'), '-CAkey', f('ca.key'), '-CAcreateserial', '-out', f('server.crt'), '-days', '30', '-extfile', f('server.ext')]);
    // the chain a server sends: leaf, then the CA
    fs.writeFileSync(f('chain.crt'), fs.readFileSync(f('server.crt')) + fs.readFileSync(f('ca.crt')));
  }
  const spki = (crt) => crypto.createHash('sha256').update(new crypto.X509Certificate(fs.readFileSync(crt)).publicKey.export({ type: 'spki', format: 'der' })).digest('base64');
  return { dir, ca: f('ca.crt'), cert: f('chain.crt'), key: f('server.key'), caPem: fs.readFileSync(f('ca.crt')),
    spki: [spki(f('server.crt')), spki(f('ca.crt'))] };
}

export function startCfSim({ port, target, certs, fakeClientIp = null, wsIdleMs = 100_000, host = '127.0.0.1' }) {
  const stats = { requests: 0, upgrades: 0, wsIdleCuts: 0 };
  const edgeHeaders = (q) => {
    const client = fakeClientIp ?? String(q.socket.remoteAddress).replace(/^::ffff:/, '');
    const h = { ...q.headers };
    for (const k of Object.keys(h)) if (k.startsWith('cf-')) delete h[k];          // the edge sets these, a client cannot
    h['cf-connecting-ip'] = client;
    h['x-forwarded-for'] = h['x-forwarded-for'] ? `${h['x-forwarded-for']}, ${client}` : client;
    h['x-forwarded-proto'] = 'https';
    h['cf-visitor'] = '{"scheme":"https"}';
    h['cf-ray'] = crypto.randomBytes(8).toString('hex') + '-AMS';
    h['cf-ipcountry'] = 'NL';
    return h;
  };
  const srv = https.createServer({ key: fs.readFileSync(certs.key), cert: fs.readFileSync(certs.cert) }, (q, s) => {
    stats.requests++;
    const up = http.request({ host: '127.0.0.1', port: target, method: q.method, path: q.url, headers: edgeHeaders(q) }, r => {
      s.writeHead(r.statusCode, { ...r.headers, server: 'cloudflare', 'cf-ray': 'sim' });
      r.pipe(s);
    });
    up.on('error', () => { if (!s.headersSent) { s.writeHead(502, { server: 'cloudflare' }); s.end('502 from cf-sim'); } else s.destroy(); });
    q.pipe(up);
  });
  // WebSockets: replay the handshake to the origin, then pipe both ways, cut when idle
  srv.on('upgrade', (q, sock, head) => {
    stats.upgrades++;
    const o = net.connect(target, '127.0.0.1', () => {
      const h = edgeHeaders(q);
      o.write(`${q.method} ${q.url} HTTP/1.1\r\n${Object.entries(h).map(([k, v]) => `${k}: ${v}`).join('\r\n')}\r\n\r\n`);
      if (head?.length) o.write(head);
      let last = Date.now();
      const touch = () => { last = Date.now(); };
      o.on('data', touch); sock.on('data', touch);
      const t = setInterval(() => { if (Date.now() - last > wsIdleMs) { stats.wsIdleCuts++; sock.destroy(); o.destroy(); } }, 1000);
      const done = () => { clearInterval(t); sock.destroy(); o.destroy(); };
      o.on('close', done); sock.on('close', done);
      o.pipe(sock); sock.pipe(o);
    });
    o.on('error', () => sock.destroy()); sock.on('error', () => o.destroy());
  });
  srv.listen(port, host);
  return { srv, stats, ready: new Promise(r => srv.listening ? r() : srv.once('listening', r)),
    close: () => new Promise(r => { srv.close(r); srv.closeAllConnections?.(); }) };
}

// A plain-HTTP proxy on this machine that sends each request to the server `route(name, path)` names (a port on
// 127.0.0.1; null: 502), by the Host's name and the path, as an edge does for one host name whose paths are on two
// boxes (PLAN D-38: / and /u2/ to Underground 2's server, /mw/ to Most Wanted's), WebSockets included.  Per request,
// not per connection: a browser sends both games' requests over one.  The client's address goes on in
// X-Forwarded-For (fakeClientIp, else the peer's); nothing else is added.
export function startRouteSim({ port, route, fakeClientIp = null, host = '127.0.0.1' }) {
  const stats = { requests: 0, upgrades: 0, byPort: {} };
  const target = q => {
    const name = String(q.headers.host ?? '').replace(/:\d+$/, '').toLowerCase();
    const t = route(name, String(q.url).split('?')[0]);
    if (t) stats.byPort[t] = (stats.byPort[t] ?? 0) + 1;
    return t;
  };
  const fwd = q => {
    const client = fakeClientIp ?? String(q.socket.remoteAddress).replace(/^::ffff:/, '');
    const h = { ...q.headers };
    h['x-forwarded-for'] = h['x-forwarded-for'] ? `${h['x-forwarded-for']}, ${client}` : client;
    return h;
  };
  const srv = http.createServer((q, s) => {
    stats.requests++;
    const t = target(q);
    if (!t) { s.writeHead(502); s.end('502 from route-sim'); return; }
    const up = http.request({ host: '127.0.0.1', port: t, method: q.method, path: q.url, headers: fwd(q) }, r => {
      s.writeHead(r.statusCode, r.headers);
      r.pipe(s);
    });
    up.on('error', () => { if (!s.headersSent) { s.writeHead(502); s.end('502 from route-sim'); } else s.destroy(); });
    q.pipe(up);
  });
  srv.on('upgrade', (q, sock, head) => {
    stats.upgrades++;
    const t = target(q);
    if (!t) { sock.end('HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n'); return; }
    const o = net.connect(t, '127.0.0.1', () => {
      o.write(`${q.method} ${q.url} HTTP/1.1\r\n${Object.entries(fwd(q)).map(([k, v]) => `${k}: ${v}`).join('\r\n')}\r\n\r\n`);
      if (head?.length) o.write(head);
      const done = () => { sock.destroy(); o.destroy(); };
      o.on('close', done); sock.on('close', done);
      o.pipe(sock); sock.pipe(o);
    });
    o.on('error', () => sock.destroy()); sock.on('error', () => o.destroy());
  });
  // (a port that is taken: `ready` fails, the caller decides)
  const ready = new Promise((res, rej) => { srv.once('listening', res); srv.once('error', rej); });
  srv.listen(port, host);
  return { srv, stats, ready,
    close: () => new Promise(r => { if (!srv.listening) return r(); srv.close(r); srv.closeAllConnections?.(); }) };
}

// ---- as a program ----
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const a = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
  const certs = makeTestCerts(a('--dir', path.join(process.env.TMPDIR || '/tmp', 'nfsu2-cf-sim')));
  const sim = startCfSim({ port: +a('--port', 8527), target: +a('--target', 8627), certs, fakeClientIp: a('--fake-client-ip', null),
    wsIdleMs: +a('--ws-idle-ms', 100_000) });
  await sim.ready;
  console.log(`cf-sim: https://127.0.0.1:${a('--port', 8527)}/ -> http://127.0.0.1:${a('--target', 8627)}/  (test CA ${certs.ca}; SPKI ${certs.spki.join(',')})`);
  process.on('SIGTERM', () => sim.close().then(() => process.exit(0)));
}
