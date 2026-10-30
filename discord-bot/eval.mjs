// The bot against past threads: each case goes through a look as a new post would (investigate.mjs, vet), with the
// earlier cases as the forum's threads, and the report puts the bot's verdict beside what staff or members answered.
//   node --env-file=.env eval.mjs <cases.json> [--limit 20] [--only id,id] [--staff] [--forum bug-reports] [--budget 5]
//     [--tags "Crash,WebGPU error,Bug"]  (the tags a look may add, as the forum would list them)
// cases.json: [{ id, forum, title, posts: [{ at, text, files: [paths relative to cases.json, or "missing:name"] }],
// reference }] (made from a Discord export; it stays out of the repository: it holds players' posts and logs).
// The report goes next to cases.json as eval-<time>.md.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { investigate, newClient, picture, vet } from './investigate.mjs';
import { Repo, defaultSource } from './repo.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, dflt) => { const i = args.indexOf(name); return i < 0 ? dflt : args.splice(i, 2)[1]; };
const flag = name => { const i = args.indexOf(name); return i >= 0 && !!args.splice(i, 1); };
const limit = Number(opt('--limit', 20)), only = opt('--only', ''), forum = opt('--forum', ''), budget = Number(opt('--budget', 5));
const staffOnly = flag('--staff');
const topics = opt('--tags', '').split(',').map(t => t.trim()).filter(Boolean);
const file = args[0];
if (!file) { console.error('usage: eval.mjs <cases.json> [--limit N] [--only id,id] [--staff] [--forum F] [--budget USD]'); process.exit(2); }

const env = process.env;
const model = env.BOT_MODEL || 'claude-sonnet-5-5', effort = env.BOT_EFFORT || 'medium';
const state = path.resolve(HERE, env.BOT_STATE || 'state');
fs.mkdirSync(state, { recursive: true });
const repo = new Repo({
  source: env.BOT_REPO || defaultSource(HERE), branch: env.BOT_BRANCH || 'main',
  dir: path.join(state, 'repo'),
  liveUrl: env.BOT_LIVE_URL || 'https://nfsu2.xyz/game.json', liveFile: env.BOT_LIVE_FILE || '/live/nfsu2.json',
});
const base = path.dirname(path.resolve(file));
const all = JSON.parse(fs.readFileSync(file, 'utf8'));
const byTime = [...all].sort((a, b) => a.posts[0].at.localeCompare(b.posts[0].at));
let cases = all.filter(c => (!forum || c.forum === forum) && (!staffOnly || c.reference.startsWith('staff')));
if (only) cases = only.split(',').map(id => all.find(c => c.id === id)).filter(Boolean);
cases = cases.slice(0, limit);

const TYPES = { '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp' };
const line = c => `- case:${c.id} #${c.forum}, ${c.posts[0].at.slice(0, 10)} ${c.title}: first post: ${c.posts[0].text.replace(/\s+/g, ' ').slice(0, 160)}`;

const client = newClient();
const out = path.join(base, `eval-${new Date().toISOString().slice(0, 16).replace(/[:T]/g, '-')}.md`);
const report = [`# Eval: ${cases.length} cases, ${model} (${effort})`, ''];
const tally = {};
let spent = 0;
for (const c of cases) {
  if (spent >= budget) { report.push(`Stopped at the budget ($${budget}).`); break; }
  const workDir = fs.mkdtempSync(path.join(state, 'eval-'));
  const lines = [`Forum: #${c.forum}`, `Title: ${c.title}`, 'Tags: none',
    ...(topics.length ? [`Tags you may add: ${topics.join(', ')}`] : []), 'Why the bot looks now: a new post', ''];
  const images = [];
  let n = 0;
  for (const p of c.posts) {
    lines.push(`[${p.at.slice(0, 16).replace('T', ' ')} UTC] player (the poster):`);
    if (p.text) lines.push(p.text);
    for (const f of p.files) {
      if (f.startsWith('missing:')) { lines.push(`  [attachment not opened: ${f.slice(8)}]`); continue; }
      const abs = path.join(base, f), data = TYPES[path.extname(f).toLowerCase()] && images.length < 4 && fs.readFileSync(abs);
      const pic = data && await picture(data);
      if (pic) { images.push(pic); lines.push(`  [picture ${images.length}: ${path.basename(f)}]`); continue; }
      const name = `${++n}-${path.basename(f).replace(/[^\w.-]+/g, '_')}`;
      fs.copyFileSync(abs, path.join(workDir, name));
      lines.push(`  [attachment: attachments/${name}, ${Math.round(fs.statSync(abs).size / 1024)} KB]`);
    }
    lines.push('');
  }
  const earlier = byTime.filter(o => o.id !== c.id && o.posts[0].at < c.posts[0].at).reverse();
  let res;
  try {
    res = await investigate({
      client, model, effort, repo, archive: null, workDir, thread: lines.join('\n'), images,
      threads: earlier.slice(0, 30).map(line).join('\n'),
      threadIndex: () => earlier.map(o => ({ line: line(o), text: `${o.title}\n${o.posts[0].text}` })),
    });
  } catch (e) {
    report.push(`## ${c.id} ${c.title}`, '', `Error: ${e.message}`, '');
    console.error(`${c.id}: ${e.message}`);
    continue;
  } finally {
    fs.rmSync(workDir, { recursive: true, force: true });
  }
  const v = res.verdict, { post, why } = vet(v, { minConfidence: env.BOT_MIN_CONFIDENCE || 'medium' });
  spent += res.cost ?? 0;
  const key = `${post ? v.action : 'nothing'}${v?.escalate ? ' + staff' : ''}${v?.handoff ? ' + handoff' : ''}`;
  tally[key] = (tally[key] ?? 0) + 1;
  console.log(`${c.id}: ${key} (${why}), $${(res.cost ?? 0).toFixed(3)}, total $${spent.toFixed(2)}`);
  report.push(`## ${c.id} ${c.title}`, '',
    `**Thread:** ${c.posts.map(p => p.text).join(' / ').replace(/\s+/g, ' ').slice(0, 300) || '(no text)'}` +
    `${c.posts.some(p => p.files.length) ? ` [${c.posts.flatMap(p => p.files).map(f => path.basename(f).replace(/^missing:/, '')).join(', ')}]` : ''}`,
    `**Then:** ${c.reference.replace(/\s+/g, ' ')}`,
    `**Bot:** ${key}, ${v?.confidence ?? '-'} confidence${v?.duplicate_of ? `, same as ${v.duplicate_of}` : ''} (${why}); ${v?.summary ?? res.log.at(-1) ?? ''}`,
    ...(topics.length ? [`**Tags:** ${v?.tags?.join(', ') || 'none'}`] : []),
    ...(post ? ['', post.split('\n').map(l => `> ${l}`).join('\n')] : []),
    ...(v?.escalate && v.notes ? ['', '**Staff note:**', '', v.notes] : []),
    '', `-# ${res.turns} turns, $${(res.cost ?? 0).toFixed(3)}`, '');
}
report.splice(1, 0, `Outcomes: ${Object.entries(tally).map(([k, n]) => `${k} ${n}`).join(', ')}; cost $${spent.toFixed(2)}.`);
fs.writeFileSync(out, report.join('\n'));
console.log(`report: ${out}`);
