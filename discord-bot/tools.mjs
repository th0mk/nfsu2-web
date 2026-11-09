// The model's tools: read-only, and only over the bot's copy of the code (repo.mjs), the thread's attachments
// ("attachments/...", saved by the bot) and the message archive.  No shell, no network, no writes: a forum post is
// written by anyone, so whatever it talks the model into stays inside these.
import { execFile } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { promisify } from 'node:util';
import { LOG_DATE, LOG_FORMAT } from './repo.mjs';

const run = promisify(execFile);
const ATT = 'attachments';
const MAX_OUT = 15_000;          // characters of one tool result: it stays in the look and is read again every turn
const REV = /^(LIVE|HEAD|[0-9a-f]{4,40})$/;

const str = d => ({ type: 'string', description: d });
const int = d => ({ type: 'integer', description: d });
const bool = d => ({ type: 'boolean', description: d });
const tool = (name, description, properties) => ({
  name, description, strict: true,
  input_schema: { type: 'object', properties, required: Object.keys(properties), additionalProperties: false },
});

export const TOOL_DEFS = [
  tool('search', 'Search with a regular expression (POSIX extended, as git grep -E) through the code, or through the ' +
    "thread's attachments when path starts with \"attachments/\". Returns path:line: text, up to 150 matches.", {
    pattern: str('the regular expression'),
    path: str('a file or folder to search in, relative to the repository root ("" = everywhere), or "attachments/"'),
    ignore_case: bool('match regardless of case'),
  }),
  tool('read', 'Read lines of a file of the code (path relative to the repository root) or of an attachment ' +
    '("attachments/<name>"). Returns numbered lines and the file\'s line count.', {
    path: str('the file'),
    start_line: int('first line to read, from 1'),
    max_lines: int('how many lines, at most 250 (0 = 120)'),
  }),
  tool('list', 'List a folder of the code ("" = the root) or "attachments/": names, with "/" after folders.', {
    path: str('the folder'),
  }),
  tool('git_log', 'The commit history, newest first: short hash, time (UTC, and as a Discord timestamp), title, and when it ' +
    'went live. LIVE is the live server\'s commit, HEAD the newest. "abc123..LIVE" lists what came after abc123 up to the ' +
    'live version (empty: abc123 is live or newer).', {
    range: str('a commit, or A..B ("" = HEAD); commits are hex hashes, LIVE or HEAD'),
    grep: str('only commits whose message matches this regular expression, case-insensitive ("" = all)'),
    path: str('only commits that touch this file or folder ("" = all)'),
    max: int('how many commits, at most 100 (0 = 30)'),
  }),
  tool('git_show', 'One commit: its full message and the files it changed, and with patch its diff (long diffs are ' +
    'cut). Also tells whether it is in the live version, and since when.', {
    rev: str('the commit: a hex hash, LIVE or HEAD'),
    patch: bool('include the diff'),
  }),
  tool('search_threads', 'Search the forum threads the bot knows (bug reports, suggestions, also from before it ' +
    'started) by regular expression over their titles, first posts and the bot\'s summaries, case-insensitive. Shows id, ' +
    'link, forum, last activity, what the bot did, title and summary, the most recently active first.', {
    pattern: str('the regular expression (JavaScript syntax)'),
  }),
  tool('discord_search', 'Search the messages of every channel of the Discord server (not only forums) from the last ' +
    'days, by regular expression, case-insensitive. Shows when, where, who and the text. Messages of the staff channel ' +
    'are marked: what they say is for you, never for a reply.', {
    pattern: str('the regular expression (JavaScript syntax)'),
    days: int('how many days back, 1 to 7'),
  }),
  tool('labels', "The page's tabs, buttons and settings that replies name most, as the page shows them in one language " +
    '(English, then that language\'s text), from web/src/app/lang/<language>.json. For a reply not in English: call it ' +
    'in your first round, with the other calls.', {
    language: str('the language, as a code (es, pt-BR, ru, hu, ...) or its English name (spanish, portuguese, ...)'),
  }),
];

// the page's labels replies name most (start page tabs, buttons, settings), for `labels`: a language's file has ~1,400
// texts, too many to read for a few, and replies in Spanish, Hungarian and Portuguese named them in English
const LABELS = ['Saves', 'Online', 'Friends & crews', 'Game', 'Display', 'Mods', 'Controls', 'Play', 'Quit game', 'Download log',
  'Cloud saves', 'Turn on', 'Load', 'Stop syncing', "Keep this browser's", 'Put back', 'Download backup', 'Restore', 'Screenshots',
  'Free roam with others', 'Your name', 'Claim', 'World', 'Main world', 'Ghost mode', "Players' cars shown", 'Frame rate', 'Auto',
  'Resolution scale', 'Full screen on Play', 'More display settings', 'More game settings',
  'Controller', 'More controller settings', 'Hotplug', 'Off', 'Rumble', 'Trigger full press',
  'Delete game data'];
const LANG_FILES = { pt: 'portuguese', es: 'spanish', pl: 'polish', ru: 'russian', tr: 'turkish', hu: 'hungarian', uk: 'ukrainian',
  cs: 'czech', sk: 'slovak', el: 'greek', fr: 'french', de: 'german', it: 'italian', nl: 'dutch', sv: 'swedish', da: 'danish',
  zh: 'chinese', ja: 'japanese', ko: 'korean', th: 'thai' };

// labels a reply in another language still named in English (es, th, ru, pt replies of 7-8 November said "F9,
// Download log"): ones of several words, which no sentence in another language has by chance
const SWAPPED = ['Download log', 'Download backup', 'Cloud saves', 'Free roam with others', 'Delete game data',
  'More controller settings', 'More display settings', 'More game settings', 'Full screen on Play', "Players' cars shown",
  'Resolution scale', 'Trigger full press', 'Stop syncing', 'Main world'];
/** a reply in `language` with those labels in English put as the page shows them in that language: its text, as is
 *  when the language has no page file or a label no translation.  repoDir: the code copy */
export function localLabels(text, language, repoDir) {
  const l = String(language ?? '').trim().toLowerCase().split(/[-_]/)[0];
  const file = LANG_FILES[l];
  if (!file || !SWAPPED.some(x => text.includes(x))) return text;
  let texts;
  try { texts = JSON.parse(fs.readFileSync(path.join(repoDir, 'web/src/app/lang', `${file}.json`), 'utf8')); } catch { return text; }
  let out = text;
  for (const x of SWAPPED) {
    const y = (texts[x] ?? '').replace(/<[^>]*>/g, '').trim();
    // not where the reply quotes the English beside the translation ("Download log ('Pobierz dziennik')")
    if (y && y !== x && !out.includes(y)) out = out.split(x).join(y);
  }
  return out;
}

function cut(s, n = MAX_OUT) {
  return s.length <= n ? s : s.slice(0, n) + `\n[... cut: ${s.length - n} more characters: read a smaller range, or search]`;
}

export class Tools {
  /** repo: Repo; workDir: this thread's attachments; archive: Archive or null; threadIndex: () => [{ line, text }] */
  constructor({ repo, workDir, archive, threadIndex }) {
    Object.assign(this, { repo, workDir, archive, threadIndex });
  }

  /** a path the model gave, as { abs, att } inside the code copy or the attachments, or throws */
  where(p, dir = false) {
    let rel = String(p ?? '').trim().replace(/\\/g, '/').replace(/^\.\/+/, '');
    if (path.isAbsolute(rel)) throw new Error('give the path relative to the repository root (or attachments/...)');
    const att = rel === ATT || rel.startsWith(ATT + '/');
    if (att) rel = rel.slice(ATT.length).replace(/^\/+/, '');
    const root = att ? this.workDir : this.repo.dir;
    const abs = path.resolve(root, rel || '.');
    const back = path.relative(root, abs);
    if (back.startsWith('..') || path.isAbsolute(back)) throw new Error('that path leaves the repository');
    if (!att && back.split(path.sep).includes('.git')) throw new Error('.git is not readable');
    if (fs.existsSync(abs)) {
      const real = fs.realpathSync(abs), realRoot = fs.realpathSync(root);
      if (real !== realRoot && !real.startsWith(realRoot + path.sep)) throw new Error('that path leaves the repository');
      if (!dir && fs.statSync(abs).isDirectory()) throw new Error(`${p} is a folder: use list`);
    } else if (!dir || rel) {
      // English is the page's source: looks kept asking for english.json
      if (!att && back.split(path.sep).join('/').startsWith('web/src/app/lang/')) {
        throw new Error(`${p}: no such file. English has no file: its texts are the keys of each other language's file ` +
          "(web/src/app/lang/portuguese.json, ...), and the page's own code holds them");
      }
      throw new Error(`${p}: no such file`);
    }
    return { abs, att, rel: back === '' ? '' : back.split(path.sep).join('/') };
  }

  async rev(r) {
    if (!REV.test(r)) throw new Error(`not a commit: ${r} (a hex hash, LIVE or HEAD)`);
    if (r !== 'LIVE') return r;
    const live = await this.repo.liveRev();
    if (!live) throw new Error('the live version is unknown right now');
    return live;
  }

  async run(name, input) {
    try {
      switch (name) {
        case 'search': return await this.search(input);
        case 'read': return this.read(input);
        case 'list': return this.list(input);
        case 'git_log': return await this.gitLog(input);
        case 'git_show': return await this.gitShow(input);
        case 'search_threads': return this.searchThreads(input);
        case 'discord_search': return this.discordSearch(input);
        case 'labels': return this.labels(input);
        default: return { error: `no tool ${name}` };
      }
    } catch (e) {
      return { error: e.message };
    }
  }

  /** LABELS in one language: "English → theirs", one a line */
  labels({ language }) {
    const l = String(language ?? '').trim().toLowerCase();
    const file = LANG_FILES[l.split(/[-_]/)[0]] ?? (Object.values(LANG_FILES).includes(l) ? l : null);
    if (!file) {
      if (/^en\b|^english$/.test(l)) return 'English is the page\'s own language: name the labels as they are.';
      throw new Error(`no page language "${language}": the page has ${Object.entries(LANG_FILES).map(([c, f]) => `${c} (${f})`).join(', ')} and English`);
    }
    const texts = JSON.parse(fs.readFileSync(path.join(this.repo.dir, 'web/src/app/lang', `${file}.json`), 'utf8'));
    const keys = Object.keys(texts);
    const of = label => {
      // a button with markup in it ("Restore<input ...>"): its text before the markup
      const key = label in texts ? label : keys.find(k => k.startsWith(`${label}<`));
      return key ? texts[key].replace(/<[^>]*>/g, '').trim() : '';
    };
    return [`The page's labels in ${file} (English → ${file}); one missing: name it in English:`,
      ...LABELS.map(x => `${x} → ${of(x) || '(missing)'}`)].join('\n');
  }

  async search({ pattern, path: p, ignore_case }) {
    if (!pattern || pattern.length > 300) throw new Error('give a pattern of 1 to 300 characters');
    const { abs, att, rel } = this.where(p, true);
    const lines = [];
    let total = 0;
    if (att) {
      let re; try { re = new RegExp(pattern, ignore_case ? 'i' : ''); } catch (e) { throw new Error(`bad pattern: ${e.message}`); }
      const files = fs.statSync(abs).isDirectory() ? fs.readdirSync(abs).map(f => path.join(abs, f)) : [abs];
      for (const f of files) {
        if (!fs.statSync(f).isFile()) continue;
        const name = `${ATT}/${path.relative(this.workDir, f)}`;
        fs.readFileSync(f, 'utf8').split('\n').forEach((l, i) => {
          if (!re.test(l)) return;
          if (++total <= 150) lines.push(`${name}:${i + 1}: ${l.slice(0, 300)}`);
        });
      }
    } else {
      const args = ['grep', '-n', '-I', '-E', '--no-color', ...(ignore_case ? ['-i'] : []), '-e', pattern, '--'];
      if (rel) args.push(rel);
      let out = '';
      try { out = await this.repo.git(args, { timeout: 30_000 }); } catch (e) {
        if (e.code === 1) return 'no matches';
        throw new Error((e.stderr || e.message).trim().split('\n')[0]);
      }
      for (const l of out.split('\n')) {
        if (!l) continue;
        if (++total <= 150) lines.push(l.length > 300 ? l.slice(0, 300) + ' [...]' : l);
      }
    }
    if (!total) return 'no matches';
    return cut(lines.join('\n') + (total > 150 ? `\n[${total - 150} more matches: narrow the path or the pattern]` : ''));
  }

  read({ path: p, start_line, max_lines }) {
    const { abs, att, rel } = this.where(p);
    const all = fs.readFileSync(abs, 'utf8').split('\n');
    const n = Math.min(max_lines > 0 ? max_lines : 120, 250);
    const from = Math.max(1, start_line || 1);
    const slice = all.slice(from - 1, from - 1 + n)
      .map((l, i) => `${String(from + i).padStart(6)}  ${l.length > 500 ? l.slice(0, 500) + ' [...]' : l}`);
    const name = att ? `${ATT}/${rel}` : rel;
    return cut(`${name}: lines ${from}-${from + slice.length - 1} of ${all.length}\n${slice.join('\n')}`);
  }

  list({ path: p }) {
    const { abs, att } = this.where(p, true);
    if (!fs.statSync(abs).isDirectory()) throw new Error(`${p} is a file: use read`);
    const names = fs.readdirSync(abs, { withFileTypes: true })
      .filter(d => att || d.name !== '.git')
      .map(d => d.isDirectory() ? d.name + '/' : att ? `${d.name} (${fs.statSync(path.join(abs, d.name)).size} bytes)` : d.name)
      .sort();
    return names.length ? cut(names.slice(0, 400).join('\n')) : '(empty)';
  }

  async gitLog({ range, grep, path: p, max }) {
    const ends = (range || 'HEAD').split('..');
    if (ends.length > 2) throw new Error('range: a commit or A..B');
    const spec = (await Promise.all(ends.map(e => this.rev(e || 'HEAD')))).join('..');
    const args = ['log', '--no-merges', LOG_DATE, `--format=${LOG_FORMAT}`, '-n', String(Math.min(max > 0 ? max : 30, 100))];
    if (grep) args.push('-i', '-E', `--grep=${grep}`);
    args.push(spec, '--');
    if (p) args.push(this.where(p, true).rel || '.');
    const out = await this.repo.withLive(await this.repo.git(args));
    return out || 'no commits';
  }

  async gitShow({ rev, patch }) {
    const r = await this.rev(rev);
    const head = await this.repo.git(['show', '--no-patch', LOG_DATE, '--format=%H%n%h %cd <t:%ct>%n%B', r]);
    const [full, ...msg] = head.trim().split('\n');
    const stat = await this.repo.git(['show', '--format=', '--stat=120', r]);
    const live = await this.repo.liveRev();
    let state = 'unknown whether it is live';
    if (live && await this.repo.has(live)) {
      try { await this.repo.git(['merge-base', '--is-ancestor', r, live]); state = 'in the live version'; }
      catch { state = 'not in the live version yet'; }
      const since = (await this.repo.liveSince([full])).get(full);
      if (since && since !== 'not live yet') state += `, ${since}`;
    }
    let out = `${msg.join('\n')}\n\n[${state}]\n${stat.trim()}`;
    if (patch) out += '\n\n' + cut(await this.repo.git(['show', '--format=', '--patch', r]), 12_000);
    return cut(out);
  }

  searchThreads({ pattern }) {
    if (!this.threadIndex) return 'no thread list here';
    if (!pattern || pattern.length > 300) throw new Error('give a pattern of 1 to 300 characters');
    let re; try { re = new RegExp(pattern, 'i'); } catch (e) { throw new Error(`bad pattern: ${e.message}`); }
    const hits = this.threadIndex().filter(t => re.test(t.text));
    if (!hits.length) return 'no threads';
    return cut(hits.slice(0, 30).map(t => t.line).join('\n') + (hits.length > 30 ? `\n[${hits.length - 30} more: narrow the pattern]` : ''));
  }

  discordSearch({ pattern, days }) {
    if (!this.archive) return 'the message archive is off';
    if (!pattern || pattern.length > 300) throw new Error('give a pattern of 1 to 300 characters');
    let re; try { re = new RegExp(pattern, 'i'); } catch (e) { throw new Error(`bad pattern: ${e.message}`); }
    const { hits, more } = this.archive.search(re, Math.min(Math.max(days || 3, 1), 7), 40);
    if (!hits.length) return 'no messages';
    const lines = hits.map(m => `${this.archive.staff(m) ? '[staff channel, private] ' : ''}` +
      `${m.at.slice(0, 16).replace('T', ' ')} #${m.channel}${m.thread ? ` / ${m.thread}` : ''} ` +
      `${m.author}: ${m.text.replace(/\s+/g, ' ').slice(0, 400)}${m.files?.length ? ` [files: ${m.files.join(', ')}]` : ''}`);
    return cut(lines.join('\n') + (more ? `\n[${more} more: narrow the pattern]` : ''));
  }
}
