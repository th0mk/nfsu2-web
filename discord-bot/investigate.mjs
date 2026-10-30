// One investigation: the thread goes to Claude with the bot's instructions (system.md), the maintainer's notes
// (knowledge.md) and the player's guide (docs/web/PLAYING.md), and Claude reads what it needs through the tools
// (tools.mjs) until it gives its verdict as JSON (VERDICT).  All three come from the bot's copy of the code, so a pushed
// change to them applies within minutes, without a new image; the bot's own system.md and knowledge.md until then.
import Anthropic from '@anthropic-ai/sdk';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { costOf } from './spend.mjs';
import { TOOL_DEFS, Tools } from './tools.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const MAX_TURNS = 16;                            // a look that needs more is cut short: the long ones cost the most
const MAX_REPEATS = 2;                           // calls made again word for word: a look going round in circles answers
// calls in a row that bring nothing (an error, no matches, a line or two): a look that stalls answers.  Looks made
// calls such as `read x`, `search /^$/ in attachments/none` or `search_threads /zzzznomatch/` to fill a turn
const MAX_MISSES = 3;

export const VERDICT = {
  type: 'object', additionalProperties: false,
  required: ['action', 'category', 'language', 'reply', 'summary', 'notes', 'escalate', 'handoff', 'duplicate_of', 'confidence', 'tags'],
  properties: {
    action: { type: 'string', enum: ['none', 'ask', 'answer'] },
    category: { type: 'string', enum: ['crash', 'graphics', 'performance', 'controls', 'online', 'saves', 'audio', 'phone', 'suggestion', 'question', 'other'] },
    language: { type: 'string', description: 'the language of the reply, as a BCP 47 tag (en, pt-BR, ...)' },
    reply: { type: 'string', description: 'the message to post; "" for none' },
    summary: { type: 'string' },
    notes: { type: 'string', description: 'for the developer; posted in the staff channel when escalate is true' },
    escalate: { type: 'boolean', description: 'the developer should look at this' },
    handoff: { type: 'boolean', description: 'a person should take the thread from here; the bot stays out of it after this reply' },
    duplicate_of: { type: 'string' },
    confidence: { type: 'string', enum: ['low', 'medium', 'high'] },
    tags: { type: 'array', items: { type: 'string' }, description: 'names from "Tags you may add" in the thread\'s header; [] for none' },
  },
};

async function systemBlocks(repo, recent) {
  const own = async f => (await repo.file(`discord-bot/${f}`)) || fs.readFileSync(path.join(HERE, f), 'utf8');
  const [rules, notes, playing] = [await own('system.md'), await own('knowledge.md'), await repo.file('docs/web/PLAYING.md')];
  const history = [
    `Live version: ${recent.live}`,
    `Newest commit: ${recent.head}`,
    `Releases the bot saw (newest first):\n${recent.releases || 'none yet'}`,
    recent.notLive ? `Commits not live yet (newest first):\n${recent.notLive}` : 'Commits not live yet: none',
    `Commits of the last days up to the live version (newest first; when made, and when they went live):\n${recent.recent || 'none'}`,
  ].join('\n\n');
  return [
    { type: 'text', text: `${rules}\n\n${notes}` },
    // kept an hour: looks are often more than 5 minutes apart, and this part (~20k tokens) is the same for all of them
    { type: 'text', text: `# The player's guide (docs/web/PLAYING.md)\n\n${playing}`, cache_control: { type: 'ephemeral', ttl: '1h' } },
    // the same for every look until the next commit or release (~5k tokens): kept an hour too, so a look reads it from
    // the cache instead of writing it again
    { type: 'text', text: `# The code's history, as of this look\n\n${history}`, cache_control: { type: 'ephemeral', ttl: '1h' } },
  ];
}

/** the first user message: what is going on (volatile, so after the cached system prompt), the pictures, the thread */
function opening({ now, threads, reports, thread, images }) {
  const ctx = [
    `Now: ${now}`,
    `Recent forum threads (id, category, what the bot did, title: summary):\n${threads || 'none yet'}`,
    `Problems the developer has a report on in the staff channel, the latest change first (the report's first thread: ` +
      `id and link; where it stands; its title):\n${reports || 'none yet'}`,
  ].join('\n\n');
  return [
    { type: 'text', text: `<context>\n${ctx}\n</context>` },
    ...images.map(im => ({ type: 'image', source: { type: 'base64', media_type: im.type, data: im.data } })),
    {
      type: 'text', text: `<discord_thread>\n${thread}\n</discord_thread>\n\n` +
        (images.length ? `The ${images.length} picture(s) above are the thread's images, in the order listed.\n\n` : '') +
        'Investigate this thread as your instructions say, then give your verdict.',
    },
  ];
}

/** a tool call in a few words, for the log: search /iphone|ios/ in attachments/, read docs/web/PLAYING.md:520 */
function brief(name, i) {
  switch (name) {
    case 'search': return `search /${i.pattern}/${i.ignore_case ? 'i' : ''} in ${i.path || 'the code'}`;
    case 'read': return `read ${i.path}:${i.start_line || 1}`;
    case 'list': return `list ${i.path || '/'}`;
    case 'git_log': return `git log ${i.range || 'HEAD'}${i.grep ? ` grep /${i.grep}/` : ''}${i.path ? ` -- ${i.path}` : ''}`;
    case 'git_show': return `git show ${i.rev}${i.patch ? ' with diff' : ''}`;
    case 'search_threads': return `threads /${i.pattern}/`;
    case 'discord_search': return `messages /${i.pattern}/ ${i.days} d`;
    default: return name;
  }
}
/** a call that brought nothing: an error, nothing found, or a line or two of a file */
function missed(name, input, out) {
  if (out?.error) return true;
  if (/^(no matches|no commits|no threads|no messages|\(empty\))$/.test(String(out))) return true;
  return name === 'read' && input.max_lines > 0 && input.max_lines <= 2;
}
/** a tool's result in a few words: how many lines or matches, or the error */
function gist(out) {
  if (out?.error) return `error: ${out.error}`;
  const text = String(out);
  if (/^(no matches|no commits|no threads|no messages|\(empty\))$/.test(text)) return text;
  const n = text.split('\n').length;
  return `${n} line${n === 1 ? '' : 's'}`;
}

/**
 * client: Anthropic; repo: Repo; archive: Archive|null; workDir: the thread's attachments;
 * thread: its text as built by the bot; images: [{type, data}]; threads: the recent-threads list as text; reports:
 * the developer's reports as text; threadIndex: () => every known thread, for search_threads; step: (text) => void,
 * told each step as it happens
 * -> { verdict, usage, cost, turns, log }
 */
export async function investigate({ client, model, effort, maxTurns = MAX_TURNS, repo, archive, workDir, thread, images = [], threads = '', reports = '', threadIndex, step = () => {} }) {
  await repo.sync();
  const tools = new Tools({ repo, workDir, archive, threadIndex });
  const recent = await repo.recent();
  const system = await systemBlocks(repo, recent);
  step(`code at ${recent.head.split(' ')[0]}, live ${recent.live.split(' ')[0]}${recent.notLive ? `, ${recent.notLive.split('\n').length} commit(s) not live yet` : ''}; asking ${model}`);
  const messages = [{ role: 'user', content: opening({ now: new Date().toISOString().slice(0, 16) + 'Z', threads, reports, thread, images }) }];
  const usage = { input: 0, output: 0, cacheWrite: 0, cacheRead: 0 };
  let cost = 0;                                  // null once a turn's model has no prices
  const log = [];
  let verdict = null;
  const seen = new Set();
  let repeats = 0, misses = 0;
  const stalled = () => repeats >= MAX_REPEATS || misses >= MAX_MISSES;

  for (let turn = 1; turn <= maxTurns && !verdict; turn++) {
    const last = turn === maxTurns || stalled();
    const res = await client.beta.messages.create({
      model, max_tokens: 16000, system, messages,
      tools: TOOL_DEFS,
      ...(last ? { tool_choice: { type: 'none' } } : {}),
      thinking: { type: 'adaptive' },
      output_config: { effort, format: { type: 'json_schema', schema: VERDICT } },
      cache_control: { type: 'ephemeral' },
      betas: ['server-side-fallback-2026-07-01'],
      fallbacks: 'default',
    });
    const u = res.usage;
    usage.input += u.input_tokens; usage.output += u.output_tokens;
    usage.cacheWrite += u.cache_creation_input_tokens ?? 0; usage.cacheRead += u.cache_read_input_tokens ?? 0;
    const c = costOf(res.model ?? model, u);
    cost = cost == null || c == null ? null : cost + c;

    if (res.stop_reason === 'refusal') { log.push(`refused (${res.stop_details?.category ?? 'no category'})`); step(log.at(-1)); break; }
    if (res.stop_reason === 'max_tokens') { log.push('ran out of output tokens'); step(log.at(-1)); break; }
    messages.push({ role: 'assistant', content: res.content });

    const calls = res.content.filter(b => b.type === 'tool_use');
    if (res.stop_reason === 'tool_use' && calls.length) {
      const results = await Promise.all(calls.map(async c => {
        const key = `${c.name} ${JSON.stringify(c.input)}`;
        if (seen.has(key)) {                     // the same call again (one look read a log's first line 7 times)
          repeats++;
          log.push(`${key} -> repeated, not run`);
          step(`${brief(c.name, c.input)} -> repeated, not run`);
          return { missed: true, result: { type: 'tool_result', tool_use_id: c.id,
            content: 'You made this same call before in this look: its result is above. Read something else, or give your verdict.' } };
        }
        seen.add(key);
        const out = await tools.run(c.name, c.input);
        const text = typeof out === 'string' ? out : JSON.stringify(out);
        log.push(`${c.name} ${JSON.stringify(c.input)} -> ${text.length} chars${out?.error ? ` (error: ${out.error})` : ''}`);
        step(`${brief(c.name, c.input)} -> ${gist(out)}`);
        return { missed: missed(c.name, c.input, out),
          result: { type: 'tool_result', tool_use_id: c.id, content: text, ...(out?.error ? { is_error: true } : {}) } };
      }));
      for (const r of results) misses = r.missed ? misses + 1 : 0;
      const content = results.map(r => r.result);
      if (turn + 1 === maxTurns || stalled()) content.push({ type: 'text', text: 'That was the last tool call: give your verdict now.' });
      messages.push({ role: 'user', content });
      continue;
    }

    const text = res.content.filter(b => b.type === 'text').map(b => b.text).join('');
    try { verdict = JSON.parse(text); } catch { log.push(`no JSON verdict: ${text.slice(0, 200)}`); step(log.at(-1)); break; }
    step(`verdict after ${turn} turn${turn === 1 ? '' : 's'}: ${verdict.action}${verdict.escalate ? ' + tell the staff' : ''}` +
      `${verdict.duplicate_of ? `, same as ${verdict.duplicate_of}` : ''} (${verdict.confidence}): ${verdict.summary}`);
  }

  return { verdict, usage, cost, turns: messages.filter(m => m.role === 'assistant').length, log };
}

/**
 * what may be posted, or why not: a verdict that says nothing, a weak answer, or a reply that pastes code is dropped
 * -> { post: string|null, why }
 */
export function vet(v, { minConfidence = 'medium' } = {}) {
  if (!v) return { post: null, why: 'no verdict' };
  if (v.action === 'none') return { post: null, why: 'none' };
  const reply = (v.reply ?? '').trim();
  if (!reply) return { post: null, why: `${v.action} without a reply` };
  const rank = { low: 0, medium: 1, high: 2 };
  if (v.action === 'answer' && rank[v.confidence] < rank[minConfidence]) return { post: null, why: `answer at ${v.confidence} confidence` };
  for (const block of reply.match(/```[\s\S]*?```/g) ?? []) {
    if (block.split('\n').length > 8) return { post: null, why: 'a long code block' };
  }
  return { post: reply, why: v.action };
}

/** how much of `reply` the `earlier` text says already: the share of its word pairs found there, 0..1 */
export function overlap(reply, earlier) {
  const pairs = s => { const w = s.toLowerCase().match(/[\p{L}\p{N}]+/gu) ?? []; return new Set(w.slice(1).map((x, i) => `${w[i]} ${x}`)); };
  const a = pairs(reply), b = pairs(earlier);
  let n = 0;
  for (const p of a) if (b.has(p)) n++;
  return a.size ? n / a.size : 0;
}

/** a reply as Discord messages of at most `max` characters: its blank-line parts (never split inside a code block),
 *  as many to a message as fit; a part too long for one is split between lines, a line between words */
export function blocks(text, max = 2000) {
  const parts = [];
  let cur = [], code = false;
  for (const line of text.split('\n')) {
    if (/^\s*```/.test(line)) code = !code;
    if (!code && !line.trim()) { if (cur.length) parts.push(cur.join('\n')); cur = []; continue; }
    cur.push(line);
  }
  if (cur.length) parts.push(cur.join('\n'));
  const lines = p => p.split('\n').flatMap(l => l.length <= max ? [l] : words(l, max));
  return pack(parts.flatMap(p => p.length <= max ? [p] : pack(lines(p), '\n', max)), '\n\n', max);
}
/** strings, each at most `max` long, joined by `sep` into as few strings of at most `max` as they fit */
function pack(items, sep, max) {
  const out = [];
  for (const s of items) {
    if (out.length && out.at(-1).length + sep.length + s.length <= max) out[out.length - 1] += sep + s;
    else out.push(s);
  }
  return out;
}
/** a line too long for a message, in pieces between words (a word too long for one, cut) */
const words = (line, max) => pack(line.split(/(?<=\s)/).flatMap(w => w.length <= max ? [w] : w.match(new RegExp(`[^]{1,${max}}`, 'g'))), '', max);

export const newClient = () => new Anthropic();

/** a picture's type from its first bytes, or null when it is none the API takes: the name or the type Discord gives
 *  can be wrong (a PNG saved as .webp), and the API refuses the whole look then */
export function imageType(b) {
  if (b.length >= 8 && b.readUInt32BE(0) === 0x89504e47 && b.readUInt32BE(4) === 0x0d0a1a0a) return 'image/png';
  if (b.length >= 3 && b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff) return 'image/jpeg';
  if (b.length >= 6 && /^GIF8[79]a$/.test(b.toString('latin1', 0, 6))) return 'image/gif';
  if (b.length >= 12 && b.toString('latin1', 0, 4) === 'RIFF' && b.toString('latin1', 8, 12) === 'WEBP') return 'image/webp';
  return null;
}

// A picture costs a visual token per 28x28 pixels, and Sonnet 5.5 reads them up to 4,784 tokens (2,576 px a side).
// The bot sends them at most at the size every Claude before Opus 4.7 read them, 1,568 tokens and 1,568 px a side: a
// page's message or a screenshot of the game reads as well, for a third of the tokens of a 4K screenshot
const PICTURE_EDGE = 1568, PICTURE_TOKENS = 1568;
const ORIGINAL_MAX = 3_500_000;                  // a picture sent as it is (5 MB the API's limit, base64)
const visualTokens = (w, h) => Math.ceil(w / 28) * Math.ceil(h / 28);
const evenRound = x => { const f = Math.floor(x); return x - f !== 0.5 ? Math.round(x) : f % 2 ? f + 1 : f; };

/** the largest size of the same shape within PICTURE_EDGE and PICTURE_TOKENS, as the API computes its own shrinking
 *  (https://platform.claude.com/docs/en/build-with-claude/vision-coordinates) */
export function fitted(w, h, edge = PICTURE_EDGE, max = PICTURE_TOKENS) {
  const fits = (a, b) => Math.ceil(a / 28) * 28 <= edge && Math.ceil(b / 28) * 28 <= edge && visualTokens(a, b) <= max;
  if (fits(w, h)) return [w, h];
  if (h > w) return fitted(h, w, edge, max).reverse();
  const ratio = w / h, short = l => Math.max(evenRound(l / ratio), 1);
  let lo = 1, hi = w;                            // lo fits, hi does not
  while (lo + 1 < hi) { const mid = (lo + hi) >> 1; if (fits(mid, short(mid))) lo = mid; else hi = mid; }
  return [lo, short(lo)];
}

/** a picture as the model gets it, { type, data (base64), tokens }: as it is when small enough, else shrunk (the
 *  first frame of an animation, turned upright).  null when it is none the API takes or does not decode: the API
 *  would refuse the whole look */
export async function picture(buf) {
  const type = imageType(buf);
  if (!type) return null;
  let sharp;
  try { sharp = (await import('sharp')).default; } catch (e) {     // no image library: as before, the picture as it is
    console.error(`[pictures] sharp: ${e.message}`);
    return buf.length <= ORIGINAL_MAX ? { type, data: buf.toString('base64'), tokens: null } : null;
  }
  try {
    const m = await sharp(buf).metadata();
    const fh = m.pages > 1 && m.pageHeight ? m.pageHeight : m.height;
    const [w, h] = (m.orientation ?? 1) >= 5 ? [fh, m.width] : [m.width, fh];
    const [tw, th] = fitted(w, h);
    if (tw === w && th === h && buf.length <= ORIGINAL_MAX) return { type, data: buf.toString('base64'), tokens: visualTokens(w, h) };
    const img = sharp(buf).rotate().resize(tw, th, { fit: 'fill' });
    const [out, data] = type === 'image/jpeg' ? ['image/jpeg', await img.jpeg({ quality: 90 }).toBuffer()]
      : type === 'image/webp' ? ['image/webp', await img.webp({ quality: 90 }).toBuffer()]
      : ['image/png', await img.png().toBuffer()];
    return { type: out, data: data.toString('base64'), tokens: visualTokens(tw, th) };
  } catch { return null; }
}
