// The virtual LAN switch's /lan upgrade on the real server, when the game folder is there: the Origin check, and
// /lan?resume giving a departed player's address back only to its token (the audit's F1):
//   node --test web/test/lan.test.mjs
// The switch itself (addresses, peer lists, routing, the limits, keepalive, bundles, the page's net code against it)
// is tested in its crate, web/server-rs/crates/lan/tests.
// Port: NFSU2_TEST_PORT (8528).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { WebSocket } from 'ws';
import { serverCommand } from '../tools/server-cmd.mjs';

const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const DATA = process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2');

test('the server: /lan accepts its own pages and tools, refuses other sites; an address only back to its token', { skip: !fs.existsSync(path.join(DATA, 'SPEED2.EXE')) && 'no game folder' }, async () => {
  const hport = +(process.env.NFSU2_TEST_PORT || 8528);
  const st = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'lantest-'));
  const srv = spawn(...serverCommand(['--data', DATA, '--http-port', String(hport), '--state', st]), { stdio: ['ignore', 'pipe', 'pipe'] });
  try {
    await new Promise((res, rej) => {
      let out = '';
      const t = setTimeout(() => rej(new Error('server did not start: ' + out)), 15000);
      srv.stdout.on('data', d => { out += d; if (out.includes('  access    :')) { clearTimeout(t); res(); } });
      srv.on('exit', c => { clearTimeout(t); rej(new Error('server exited ' + c + ' ' + out)); });
    });
    const open = (headers, q = '', first = null, keep = false) => new Promise((res, rej) => {
      const ws = new WebSocket(`ws://127.0.0.1:${hport}/lan${q}`, { headers });
      setTimeout(() => rej(new Error(`no welcome and no close in 5 s (${q || 'no query'})`)), 5000).unref();
      if (first) ws.on('open', () => ws.send(JSON.stringify(first)));
      ws.on('message', (d, bin) => { if (!bin) { const m = JSON.parse(String(d)); if (m.t === 'welcome') { res(keep ? { ...m, ws } : m); if (!keep) ws.close(); } } });
      ws.on('error', () => res(null)); ws.on('unexpected-response', () => res(null)); ws.on('close', () => res(null));
    });
    assert.ok(await open({}), 'no Origin (a tool): accepted');
    assert.ok(await open({ origin: `http://127.0.0.1:${hport}` }), 'the server\'s own page: accepted');
    assert.equal(await open({ origin: 'https://evil.example' }), null, 'another site: refused');
    // an address back only with its token
    const a = await open({}, '', null, true);
    const vip = a.vipStr;
    a.ws.close();
    await sleep(100);
    const thief = await open({}, `?vip=${vip}&resume`, { t: 'hello', name: 'thief', vip, token: 'A'.repeat(22) });
    assert.notEqual(thief.vipStr, vip, '?vip= or a wrong token: never the departed player\'s address');
    const back = await open({}, '?resume', { t: 'hello', name: 'player', vip, token: a.token });
    assert.equal(back.vipStr, vip, 'its own token: the address back');
  } finally {
    // the server saves its addresses as it stops: remove the folder only after it is gone
    const gone = srv.exitCode !== null || srv.signalCode !== null ? null : new Promise(res => srv.once('exit', res));
    srv.kill(); await gone;
    fs.rmSync(st, { recursive: true, force: true });
  }
});
