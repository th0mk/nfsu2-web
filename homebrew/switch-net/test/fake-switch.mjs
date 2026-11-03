// A fake switch for n2n_cli: the part of the server's /lan protocol the client uses (web/server-rs/crates/lan),
// with checks on what the client sends.  node fake-switch.mjs <port>; exits 0 after one client that sent what was
// expected (a masked hello without compact/delta, a ping, a frame to the hub), 1 otherwise.
import http from 'node:http';
import crypto from 'node:crypto';

const port = +process.argv[2] || 8099;
const problems = [];
const need = (ok, what) => { if (!ok) { problems.push(what); console.error('FAIL: ' + what); } };

const frame = (op, payload) => {
  const n = payload.length;
  const h = n < 126 ? Buffer.from([0x80 | op, n]) : n < 65536 ? Buffer.from([0x80 | op, 126, n >> 8, n & 255]) : null;
  return Buffer.concat([h, payload]);                      // a server sends its frames unmasked
};
const text = (o) => frame(1, Buffer.from(typeof o === 'string' ? o : JSON.stringify(o)));
const u32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32BE(v >>> 0); return b; };
const udp = (src, dst, sport, dport, data) =>
  Buffer.concat([Buffer.from([1, 0, 0, 0]), u32(src), u32(dst), Buffer.from([sport >> 8, sport & 255, dport >> 8, dport & 255]), Buffer.from(data)]);

const OWN = 0x0A4D0105, FRIEND = 0x0A4D0106, SERVER = 0x0A4D0001, HUB = 0x0A4D00FD;

const srv = http.createServer((q, r) => { r.writeHead(404); r.end(); });
srv.on('upgrade', (req, sock) => {
  need(/^\/lan\?queue$/.test(req.url), `target is /lan?queue, got ${req.url}`);
  const key = req.headers['sec-websocket-key'];
  need(!!key && Buffer.from(key, 'base64').length === 16, 'Sec-WebSocket-Key is 16 bytes in base64');
  need(req.headers['sec-websocket-version'] === '13', 'version 13');
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);

  let buf = Buffer.alloc(0), hello = false, gotPing = false, gotHub = false;
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      if (buf.length < 2) return;
      const op = buf[0] & 15, masked = buf[1] >> 7; let len = buf[1] & 127, at = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); at = 4; }
      if (buf.length < at + 4 + len) return;
      need(masked === 1, 'client frames are masked');
      const mask = buf.subarray(at, at + 4), body = Buffer.from(buf.subarray(at + 4, at + 4 + len));
      for (let i = 0; i < len; i++) body[i] ^= mask[i & 3];
      buf = buf.subarray(at + 4 + len);
      if (op === 1) onText(JSON.parse(body.toString()));
      else if (op === 2) onBinary(body);
      else if (op === 8) { sock.write(frame(8, Buffer.from([3, 232]))); sock.end(); finish(); return; }
    }
  });
  sock.on('error', () => {});

  function onText(m) {
    if (m.t === 'hello') {
      need(!hello, 'one hello');
      hello = true;
      need(m.name === 'Zé "Q" \\ ünï', `the name arrives escaped and intact, got ${JSON.stringify(m.name)}`);
      need(m.id === 'n2n-cli-id' && m.lan === 0 && m.bundle === 1, 'hello has id, lan 0, bundle 1');
      need(!('compact' in m) && !('delta' in m) && !('fpred' in m) && !('ldelta' in m), 'hello asks for no compact/delta');
      sock.write(text({ t: 'welcome', vip: OWN, vipStr: '10.77.1.5', mask: '255.255.192.0', token: 'tok123', bundle: 1 }));
      sock.write(text('{"t":"peers","peers":[{"vip":' + SERVER + ',"name":"Online","server":true},'
        + '{"vip":' + FRIEND + ',"name":"Jos\\u00e9 \\ud83c\\udfce"},{"vip":' + OWN + ',"name":"me"}]}'));
      // a bundle of two frames from the friend (n = 0 entries: [0] 0 [1] 0 [2..3] length, the frame)
      const f1 = udp(FRIEND, OWN, 47210, 47210, 'AAAA'), f2 = udp(FRIEND, OWN, 47210, 47211, 'BBBBBB');
      const ent = (f) => Buffer.concat([Buffer.from([0, 0, f.length >> 8, f.length & 255]), f]);
      sock.write(frame(2, Buffer.concat([Buffer.from([32, 0, 0, 0]), ent(f1), ent(f2)])));
      sock.write(frame(2, udp(FRIEND, OWN, 47210, 47210, 'CC')));                  // and one frame alone
      // a text message split in two fragments, with a ping between them
      const t = Buffer.from('{"t":"delta","peers":[{"vip":' + (FRIEND + 1) + ',"name":"New"}],"gone":[' + FRIEND + ']}');
      sock.write(Buffer.concat([Buffer.from([0x01, 10]), t.subarray(0, 10), frame(9, Buffer.from('p')), Buffer.from([0x80, t.length - 10]), t.subarray(10)]));
    } else if (m.t === 'ping') {
      gotPing = true;
      sock.write(text({ t: 'pong', id: m.id }));
    }
  }
  function onBinary(b) {
    need(b.length === 18 && b[0] === 1 && b.readUInt32BE(8) === HUB && b.readUInt16BE(12) === 1234 && b.readUInt16BE(14) === 47210
      && b.subarray(16).toString() === 'hi', `the frame to the hub: ${b.toString('hex')}`);
    gotHub = true;
    sock.write(text({ t: 'kick', msg: 'bye "now"' }));
    sock.write(frame(8, Buffer.from([0x0F, 0xA1])));                                  // 4001
  }
  function finish() {
    need(hello, 'a hello came'); need(gotPing, 'a ping came'); need(gotHub, 'the frame to the hub came');
    console.log(problems.length ? 'FAKE: FAILED' : 'FAKE: OK');
    process.exit(problems.length ? 1 : 0);
  }
  sock.on('close', () => { if (!sock.destroyed) return; setTimeout(finish, 50); });
});
srv.listen(port, '0.0.0.0', () => console.log('listening ' + port));
setTimeout(() => { console.error('FAKE: timeout'); process.exit(1); }, 30000).unref();
