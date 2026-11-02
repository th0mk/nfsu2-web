// The bot's own copy of the code: a clone of BOT_REPO (default: the checkout this folder is in) at BOT_BRANCH, fetched
// again before an investigation when it is more than 5 minutes old.  The model reads only this copy, never a working
// tree that someone is editing.  The live server's version (`rev`, the module's commit) comes from the module's
// nfsu2.json when the bot runs next to the server (BOT_LIVE_FILE, mounted read-only), or the image's commit from the
// release.json release.sh writes beside it (a release of only the page or the server keeps the module), else from the
// page's game.json (BOT_LIVE_URL), which Cloudflare may refuse to a server's address.  Each new live version goes into a
// release history (releasesFile), so the model can tell a player when a fix went live: at the file's time (release.sh
// writes it as the release goes out), or when the bot first saw it.
import { execFile, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { promisify } from 'node:util';

const run = promisify(execFile);
const FETCH_EVERY_MS = 5 * 60_000;
const LIVE_EVERY_MS = 60_000;
const KEEP_RELEASES_MS = 30 * 86400_000;
const GIT_ENV = { ...process.env, TZ: 'UTC' };   // dates in the model's context and tools are UTC

/** where the code comes from without BOT_REPO: the checkout the bot's folder is in */
export function defaultSource(here) {
  try {
    return execFileSync('git', ['-C', here, 'rev-parse', '--show-toplevel'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
  } catch {
    throw new Error('set BOT_REPO: this folder is not in a git checkout (README.md)');
  }
}

export class Repo {
  constructor({ source, branch, dir, liveUrl, liveFile, releasesFile }) {
    Object.assign(this, { source, branch, dir, liveUrl, liveFile, releasesFile });
    this.fetchedAt = 0;
    this.live = { rev: null, at: 0, via: null };
    /** the live versions the bot saw, oldest first: [{ rev, at }], at in ms, null when unknown */
    this.releases = [];
    try { this.releases = JSON.parse(fs.readFileSync(releasesFile, 'utf8')); } catch { /* none yet */ }
  }

  async git(args, { maxBuffer = 32 << 20, timeout = 30_000 } = {}) {
    const { stdout } = await run('git', ['-C', this.dir, ...args], { maxBuffer, timeout, env: GIT_ENV });
    return stdout;
  }

  /** clone on first use, then fetch + reset when stale (the model never sees a half-updated tree: one job at a time);
   *  calls while one runs wait for it */
  sync(force = false) {
    this.syncing ??= this.syncNow(force).finally(() => { this.syncing = null; });
    return this.syncing;
  }

  async syncNow(force) {
    if (!fs.existsSync(path.join(this.dir, '.git'))) {
      fs.mkdirSync(path.dirname(this.dir), { recursive: true });
      await run('git', ['clone', '--quiet', '--single-branch', '--branch', this.branch, this.source, this.dir],
        { timeout: 600_000 });
      this.fetchedAt = Date.now();
      return;
    }
    if (!force && Date.now() - this.fetchedAt < FETCH_EVERY_MS) return;
    try {
      await this.git(['remote', 'set-url', 'origin', this.source]);   // BOT_REPO may have changed since the clone
      await this.git(['fetch', '--quiet', 'origin', `+refs/heads/${this.branch}:refs/remotes/origin/${this.branch}`],
        { timeout: 120_000 });
      await this.git(['reset', '--quiet', '--hard', `origin/${this.branch}`]);
      await this.git(['clean', '-qfdx']);
      this.fetchedAt = Date.now();
    } catch (e) {
      console.error(`[repo] fetch failed, using the copy from ${new Date(this.fetchedAt).toISOString()}: ${e.message}`);
    }
  }

  /** the live server's commit (short hash), or null when neither the module file nor the page gives it */
  async liveRev() {
    if (Date.now() - this.live.at < LIVE_EVERY_MS) return this.live.rev;
    const rev = j => /^[0-9a-f]{4,40}$/.test(j?.rev ?? '') ? j.rev : null;
    let found = null, via = null, stamp = this.liveFile;
    if (this.liveFile && fs.existsSync(this.liveFile)) {
      try { found = rev(JSON.parse(fs.readFileSync(this.liveFile, 'utf8'))); via = 'file'; }
      catch (e) { console.error(`[repo] live version from ${this.liveFile}: ${e.message}`); }
      // release.sh's release.json beside it names the running image's commit, a later one after a release of only the
      // page or the server; only while it names this module (one copied in by hand leaves it stale)
      const file = path.join(path.dirname(this.liveFile), 'release.json');
      try {
        const j = JSON.parse(fs.readFileSync(file, 'utf8'));
        if (found && j.module === found && rev({ rev: j.image })) { found = j.image; stamp = file; }
      } catch (e) { if (e.code !== 'ENOENT') console.error(`[repo] ${file}: ${e.message}`); }
    } else if (this.liveFile && !this.saidNoFile) {
      this.saidNoFile = true;
      console.error(`[repo] no ${this.liveFile}: mount the live module's folder there (compose.yml: /srv/nfsu2/module:/live:ro); trying the page`);
    }
    if (!found && this.liveUrl) {
      try {
        const r = await fetch(this.liveUrl, { signal: AbortSignal.timeout(10_000), headers: { 'cache-control': 'no-cache', accept: 'application/json' } });
        const text = await r.text();
        if (!text.trimStart().startsWith('{')) throw new Error(`HTTP ${r.status}, not JSON (a Cloudflare check?)`);
        found = rev(JSON.parse(text)); via = 'page';
      } catch (e) { console.error(`[repo] live version from ${this.liveUrl}: ${e.message}`); }
    }
    this.live = { rev: found, at: Date.now(), via: found ? via : null };
    if (found) {
      let mtime = null;
      if (via === 'file') try { mtime = fs.statSync(stamp).mtimeMs; } catch { /* gone just now */ }
      this.noteRelease(found, mtime);
    }
    return found;
  }

  /** a live version other than the last one goes into the history: released at the module file's time, unless that
   *  is older than the last release (an undo brings back an older file), else when the bot saw it first; unknown for
   *  the very first version, seen through the page */
  noteRelease(rev, mtime) {
    const last = this.releases.at(-1), now = Date.now();
    if (last?.rev === rev) return;
    const at = mtime && mtime <= now && mtime > (last?.at ?? 0) ? Math.round(mtime) : last ? now : null;
    this.releases.push({ rev, at });
    this.releases = this.releases.filter((r, i, a) => i === a.length - 1 || (r.at ?? 0) > now - KEEP_RELEASES_MS);
    console.log(`[repo] live version ${rev}, released ${at ? new Date(at).toISOString() : 'at an unknown time'}`);
    if (!this.releasesFile) return;
    try {
      fs.writeFileSync(this.releasesFile + '.tmp', JSON.stringify(this.releases));
      fs.renameSync(this.releasesFile + '.tmp', this.releasesFile);
    } catch (e) { console.error(`[repo] ${this.releasesFile}: ${e.message}`); }
  }

  /** when each of these commits (full hashes) went live: Map(hash -> text), "not live yet", "live since <time>": the
   *  release that brought it in the last unbroken run of releases that have it, "or earlier" when that run reaches back
   *  past the history.  Empty when the live version is unknown */
  async liveSince(hashes) {
    const out = new Map(), h = this.releases, live = h.at(-1);
    if (!hashes.length || !this.live.rev || live?.rev !== this.live.rev || !(await this.has(live.rev))) return out;
    const revs = async (...a) => new Set((await this.git(['rev-list', ...a])).split('\n').filter(Boolean));
    const notLive = await revs(`${live.rev}..HEAD`);
    for (const c of hashes) if (notLive.has(c)) out.set(c, 'not live yet');
    let left = hashes.filter(c => !notLive.has(c)), i = h.length - 1;
    for (; i > 0 && left.length; i--) {
      if (!(await this.has(h[i - 1].rev))) break;  // a release that is not in this copy: what came before is unknown
      const came = await revs(h[i].rev, `^${h[i - 1].rev}`);   // what this release brought
      for (const c of left) if (came.has(c)) out.set(c, `live since ${when(h[i].at)}`);
      left = left.filter(c => !came.has(c));
    }
    for (const c of left) out.set(c, h[i].at ? `live since ${when(h[i].at)} or earlier` : 'live (since when unknown)');
    return out;
  }

  /** git log lines in LOG_FORMAT, as the model reads them: short hash, time, title, and when it went live */
  async withLive(raw) {
    const lines = raw.trim().split('\n').filter(Boolean).map(l => l.split('\t'));
    const since = await this.liveSince(lines.map(([full]) => full));
    return lines.map(([full, rest]) => `${rest}${since.has(full) ? ` · ${since.get(full)}` : ''}`).join('\n');
  }

  /** the releases of the history, newest first, as the model reads them */
  releaseList(n = 15) {
    return this.releases.slice(-n).reverse().map(r => `${r.rev} ${r.at ? `went live ${when(r.at)}` : '(when unknown)'}`).join('\n');
  }

  async has(rev) {
    try { await this.git(['cat-file', '-e', `${rev}^{commit}`]); return true; } catch { return false; }
  }

  async file(rel) {
    try { return fs.readFileSync(path.join(this.dir, rel), 'utf8'); } catch { return ''; }
  }

  /** a file as it is in a commit, '' when it is not there */
  async fileAt(rev, rel) {
    try { return await this.git(['show', `${rev}:${rel}`]); } catch { return ''; }
  }

  /** what changed lately, as the model's starting point: the live commit, what is not live yet, the last days */
  async recent(days = 4) {
    const fmt = ['--no-merges', LOG_DATE, `--format=${LOG_FORMAT}`];
    const log = async (...a) => this.withLive(await this.git(['log', ...fmt, ...a]));
    const live = await this.liveRev();
    const head = await log('-1');
    const known = live && await this.has(live);
    const out = { head, live: live ? `${live} (not in this copy of the code)` : 'unknown (the bot could not read it just now)', releases: this.releaseList() };
    if (known) {
      out.liveRev = live;
      out.live = await log('-1', live);
      out.notLive = (await this.git(['log', '--no-merges', LOG_DATE, '--format=%h %cd %s', '-n', '40', `${live}..HEAD`])).trim();
      // the days before the live commit, not before now: the list stays the same until the next release, and so does
      // the cached system prompt it is part of (investigate.mjs)
      const made = Number((await this.git(['log', '-1', '--format=%ct', live])).trim());
      out.recent = await log('-n', '80', `--since=${new Date((made - days * 86400) * 1000).toISOString()}`, live);
    } else {
      out.recent = await log('-n', '80', `--since=${days} days ago`);
    }
    return out;
  }
}

// a commit as the model reads it: its time in UTC with the Discord timestamp to quote it by, then when it went live
export const LOG_DATE = '--date=format-local:%Y-%m-%d %H:%M UTC';
export const LOG_FORMAT = '%H%x09%h %cd <t:%ct> %s';
/** a time in UTC and as a Discord timestamp: 2026-10-26 14:05 UTC <t:1761487500> */
export const when = ms => `${new Date(ms).toISOString().slice(0, 16).replace('T', ' ')} UTC <t:${Math.round(ms / 1000)}>`;
