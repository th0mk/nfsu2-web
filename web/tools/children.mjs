// The child processes of a test tool (its own server, roam-crowd, a Chrome it started itself) go when the tool ends,
// however it ends: at the end, on an error, on Ctrl-C.  A server left running holds its ports, and the next run's own
// server then fails with "server exited".
//   const c = own(spawn(...));   ...   await stop(c);
const live = new Set();

/** c goes with this process */
export function own(c) {
  live.add(c);
  c.once('exit', () => live.delete(c));
  return c;
}

/** whether c has ended: its 'exit' came already, and is not sent again (waiting for it would wait for ever) */
export const ended = c => c.exitCode !== null || c.signalCode !== null;

/** stop c and wait until it has gone (SIGKILL after 10 s); at once when it ended already */
export async function stop(c, signal = 'SIGTERM') {
  if (!c || ended(c)) return;
  const gone = new Promise(r => c.once('exit', r));
  c.kill(signal);
  const t = setTimeout(() => { if (!ended(c)) c.kill('SIGKILL'); }, 10_000);
  await gone;
  clearTimeout(t);
}

const killAll = () => { for (const c of live) { try { c.kill('SIGTERM'); } catch {} } };
process.on('exit', killAll);
for (const [sig, n] of [['SIGINT', 2], ['SIGTERM', 15], ['SIGHUP', 1]]) process.on(sig, () => { killAll(); process.exit(128 + n); });
