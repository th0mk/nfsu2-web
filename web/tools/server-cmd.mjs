// The server the tests and tools start: the Rust binary out/web/server-rs/nfsu2-server, built by
// web/server-rs/build.sh, or $NFSU2_SERVER_BIN (another build of it: another worktree's, an older one as a negative
// control):
//   spawn(...serverCommand(args), opts)
// Its defaults (the page, the module, the state folder) are this checkout's: NFSU2_REPO, set here for the children
// unless it is set already (the binary's own default is the checkout it was built in).  Its game is the one the tool
// names with --game, else UG2: a NFSU2_GAME in the shell is dropped here, so it never reaches the children (no
// --game ug2 is added: a server built before Most Wanted refuses --game).
// On Windows the binary is Linux's, so it runs in WSL: wsl.exe -e (the default distribution, or $NFSU2_WSL_DISTRO),
// with the binary and every argument that is a Windows path as WSL sees them, and NFSU2_REPO and the NFSU2_<OPTION>
// variables passed through WSLENV.  Killing wsl.exe stops the server in WSL too.
// The checkout's own binary older than its sources (web/server-rs's src folders, the Cargo files) would test the
// server of before: the tests (node --test) refuse it, the tools say so.  $NFSU2_SERVER_BIN is taken as it is.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WIN = process.platform === 'win32';
export const SERVER_BIN = process.env.NFSU2_SERVER_BIN || path.join(REPO, 'out/web/server-rs/nfsu2-server');
process.env.NFSU2_REPO ||= REPO;
delete process.env.NFSU2_GAME;

if (WIN) {
  // NFSU2_<OPTION> of the options with a value (/p: a path, translated), and the server's knobs
  const paths = ['REPO', 'DATA', 'DIST', 'WASM', 'STATE', 'TLS_CERT', 'TLS_KEY', 'ARTIFACTS', 'ONLINE_TRACE'];
  const plain = ['PORT', 'HTTP_PORT', 'BIND', 'TRUSTED_PROXIES', 'REAL_IP_HEADER', 'MAX_PEERS', 'LAN_IMPAIR', 'HOST',
    'ONLINE', 'ROAM_HUB', 'THREADS', 'BUNDLE_MS', 'BUNDLE_MAX_MS', 'BUNDLE_BUDGET', 'LAN_SNDBUF', 'SESSIONS',
    'COMPRESS_THREADS'];
  const have = (process.env.WSLENV || '').split(':').filter(Boolean);
  process.env.WSLENV = [...new Set([...have, ...paths.map(n => `NFSU2_${n}/p`), ...plain.map(n => `NFSU2_${n}`)])].join(':');
}

/** a Windows path as WSL sees it (C:\x -> /mnt/c/x, \\wsl.localhost\<distro>\x -> /x); anything else as it is */
export function wslPath(p) {
  const s = String(p);
  const drive = /^([A-Za-z]):[\\/](.*)$/.exec(s);
  if (drive) return `/mnt/${drive[1].toLowerCase()}/${drive[2].replace(/\\/g, '/')}`;
  const share = /^[\\/]{2}(?:wsl\.localhost|wsl\$)[\\/][^\\/]+[\\/]?(.*)$/i.exec(s);
  return share ? '/' + share[1].replace(/\\/g, '/') : s;
}

/** a source of the server newer than bin, or '' (the checkout's own binary only) */
let staleChecked = null;
export function newerServerSource(bin = SERVER_BIN) {
  if (staleChecked !== null) return staleChecked;
  staleChecked = '';
  const built = !process.env.NFSU2_SERVER_BIN && !WIN && fs.statSync(bin, { throwIfNoEntry: false })?.mtimeMs;
  if (!built) return staleChecked;
  const rs = path.join(REPO, 'web/server-rs');
  const dirs = [path.join(rs, 'src'), ...fs.readdirSync(path.join(rs, 'crates')).map(c => path.join(rs, 'crates', c, 'src'))];
  const files = [path.join(rs, 'Cargo.toml'), path.join(rs, 'Cargo.lock'), ...fs.readdirSync(path.join(rs, 'crates')).map(c => path.join(rs, 'crates', c, 'Cargo.toml'))];
  for (const d of dirs) if (fs.existsSync(d)) for (const f of fs.readdirSync(d, { recursive: true })) files.push(path.join(d, String(f)));
  let at = built;
  for (const f of files) {
    const st = fs.statSync(f, { throwIfNoEntry: false });
    if (st?.isFile() && st.mtimeMs > at) { at = st.mtimeMs; staleChecked = path.relative(REPO, f); }
  }
  return staleChecked;
}

/** [command, argv] that starts the server with args */
export function serverCommand(args, bin = SERVER_BIN) {
  const distro = process.env.NFSU2_WSL_DISTRO;
  // where this machine sees the binary (on Windows a WSL path is looked for through its share, if the distribution
  // is named)
  const seen = !WIN || !bin.startsWith('/') ? bin : distro ? `\\\\wsl.localhost\\${distro}${bin.replace(/\//g, '\\')}` : '';
  if (seen && !fs.existsSync(seen)) {
    throw new Error(`no server at ${bin}: build it with web/server-rs/build.sh${WIN ? ' (in WSL)' : ''}, or set NFSU2_SERVER_BIN`);
  }
  const newer = bin === SERVER_BIN ? newerServerSource(bin) : '';
  if (newer) {
    const msg = `${path.relative(REPO, bin)} is older than ${newer}: build it again with web/server-rs/build.sh (or set NFSU2_SERVER_BIN)`;
    if (process.env.NODE_TEST_CONTEXT) throw new Error(msg);
    console.error(`server-cmd: warning: ${msg}`);
  }
  if (!WIN) return [bin, args];
  // WSLENV hands WSL every name it lists, an unset one as empty: drop those, then the server takes the shell's place
  const unsetEmpty = `for v in $(env | grep -o '^NFSU2_[A-Z_]*=$' | tr -d =); do unset "$v"; done; exec "$@"`;
  return ['wsl.exe', [...(distro ? ['-d', distro] : []), '-e', 'sh', '-c', unsetEmpty, 'sh', wslPath(bin), ...args.map(wslPath)]];
}
