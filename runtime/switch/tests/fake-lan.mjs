// fake-lan.mjs -- a fake "switch" (the server's /lan) for lan_test.c: it welcomes the client, sends it frames, and checks what
// the client's rings produced.  node fake-lan.mjs <port>; prints "FAKE: OK" and exits 0 when everything it expected came.
import http from 'node:http';
import crypto from 'node:crypto';

const port = +process.argv[2] || 8098;
const problems = [];
const need = (ok, what) => { if (!ok) { problems.push(what); console.error('FAIL: ' + what); } };

const frame = (op, payload) => {
  const n = payload.length;
  const h = n < 126 ? Buffer.from([0x80 | op, n]) : Buffer.from([0x80 | op, 126, n >> 8, n & 255]);
  return Buffer.concat([h, payload]);
};
const text = (o) => frame(1, Buffer.from(typeof o === 'string' ? o : JSON.stringify(o)));
const u32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0); return b; };
const mk = (kind, src, dst, sport, dport, data) =>
  Buffer.concat([Buffer.from([kind, 0, 0, 0]), u32(src), u32(dst), Buffer.from([sport >> 8, sport & 255, dport >> 8, dport & 255]), Buffer.from(data)]);

const OWN = 0x0A4D0105, FRIEND = 0x0A4D0106, SERVER = 0x0A4D0001, HUB = 0x0A4D00FD, BCAST = 0xFFFFFFFF;
let hello = null, gotRoam = false, gotLan = false, gotLanOn = false;

const srv = http.createServer((q, r) => { r.writeHead(404); r.end(); });
srv.on('upgrade', (req, sock) => {
  need(/^\/lan\?queue$/.test(req.url), `target is /lan?queue, got ${req.url}`);
  const key = req.headers['sec-websocket-key'];
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  let buf = Buffer.alloc(0);
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      if (buf.length < 2) return;
      const op = buf[0] & 15; let len = buf[1] & 127, at = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); at = 4; }
      if (buf.length < at + 4 + len) return;
      const mask = buf.subarray(at, at + 4), body = Buffer.from(buf.subarray(at + 4, at + 4 + len));
      for (let i = 0; i < len; i++) body[i] ^= mask[i & 3];
      buf = buf.subarray(at + 4 + len);
      if (op === 1) onText(JSON.parse(body.toString()));
      else if (op === 2) onBinary(body);
      else if (op === 8) { sock.write(frame(8, Buffer.from([3, 232]))); sock.end(); }
    }
  });
  sock.on('error', () => {});
  function onText(m) {
    if (m.t === 'hello') {
      if (!hello) hello = m;
      sock.write(text({ t: 'welcome', vip: OWN, mask: '255.255.255.0', token: 'tok123', bundle: 1 }));
      sock.write(text('{"t":"peers","peers":[{"vip":' + SERVER + ',"name":"Online","server":true},{"vip":' + FRIEND + ',"name":"Friend"},{"vip":' + OWN + ',"name":"me"}]}'));
      // free roam's datagram from the friend, and a LAN broadcast with its 8-byte header (op 0 = UDP data)
      sock.write(frame(2, mk(1, FRIEND, OWN, 47210, 47210, 'ROAM')));
      sock.write(frame(2, mk(3, FRIEND, BCAST, 3658, 3658, Buffer.concat([Buffer.from([0, 64, 0, 0, 0, 0, 0, 0]), Buffer.from('LAN!')]))));
    } else if (m.t === 'lan') {
      if (m.on === 1) gotLanOn = true;
    }
  }
  function onBinary(b) {
    const kind = b[0], src = b.readUInt32BE(4), dst = b.readUInt32BE(8), sp = b.readUInt16BE(12), dp = b.readUInt16BE(14), data = b.subarray(16).toString();
    console.log(`frame kind ${kind} ${src.toString(16)} -> ${dst.toString(16)} ${sp}->${dp} ${JSON.stringify(data)}`);
    if (kind === 1 && dst === HUB && sp === 47210 && dp === 47210 && data === 'to-hub' && src === OWN) gotRoam = true;
    if (kind === 3 && dst === BCAST && dp === 3658 && data.endsWith('lan-out')) gotLan = true;
  }
  setTimeout(() => {
    need(hello && hello.name === 'Switch' && typeof hello.id === 'string' && hello.id.length >= 16, 'hello has the name and the id: ' + JSON.stringify(hello));
    need(hello && hello.lan === 1, 'hello says LAN play is open');
    need(gotRoam, 'the free-roam datagram to the hub arrived with our vip as the source');
    need(gotLan, 'the LAN broadcast arrived');
    console.log(problems.length ? 'FAKE: FAILED' : 'FAKE: OK');
    process.exit(problems.length ? 1 : 0);
  }, 6000).unref?.();
});
srv.listen(port, '127.0.0.1', () => console.log('listening ' + port));
setTimeout(() => { console.error('FAKE: timeout'); process.exit(1); }, 30000).unref();
