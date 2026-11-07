// The forum bot: reads every channel it can see (archive.mjs), and answers only in forum threads, where a new post
// gets one look (investigate.mjs) once its author has stopped adding to it.  It says something only when it has a
// verified fix or needs information the post lacks; after it asked, the author's next post gets one more look.  It
// stays out of threads the staff answered and stops after BOT_MAX_REPLIES replies in a thread.  What a look finds for the
// developer (notes, a flag that a person should look, the threads that are the same problem, the crash) is kept on the
// thread in state/threads.json, which the discord-history MCP reads; nothing is posted for staff.  When a release goes live,
// threads whose problem one of its commits fixes get a look ('fix live'), and a careful "may be fixed, reload" when the
// look is sure; suggestions one of its commits carries out, also old ones, get 'implemented': "added, reload", Complete.
//   node --env-file=.env bot.mjs        (settings: README.md)
import { ChannelType, Client, Events, GatewayIntentBits, Partials, PermissionFlagsBits, Routes } from 'discord.js';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Archive } from './archive.mjs';
import { matchFeatures, matchFixes } from './fixes.mjs';
import { General, scriptsOf } from './general.mjs';
import { Help } from './help.mjs';
import { COMMAND as FAQ, Faq, PREFIX as FAQ_BUTTON } from './faq.mjs';
import { blocks, firstPass, investigate, newClient, overlap, picture, replyPass, vet } from './investigate.mjs';
import { Repo, defaultSource } from './repo.mjs';
import { logGist } from './logs.mjs';
import { crashOf, folderTops, samePlace } from './sigs.mjs';
import { Spend, metered } from './spend.mjs';
import { Tags } from './tags.mjs';
import { COMMAND as TRANSLATE, Translator } from './translate.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const env = process.env;
const list = s => (s ?? '').split(/[\s,]+/).filter(Boolean);
const CFG = {
  token: env.DISCORD_TOKEN,
  forums: list(env.BOT_FORUMS),                  // forum channel ids; empty: every forum
  staff: list(env.BOT_STAFF),                    // user or role ids, besides the owner and whoever can manage threads
  staffChannel: env.BOT_STAFF_CHANNEL || '',     // a private staff channel: never watched; its archived messages are marked private
  model: env.BOT_MODEL || 'claude-sonnet-5-5',
  effort: env.BOT_EFFORT || 'medium',
  dryRun: env.BOT_DRY_RUN === '1',
  waitS: Number(env.BOT_WAIT_S || 120),
  maxReplies: Number(env.BOT_MAX_REPLIES || 2),
  maxTalk: Number(env.BOT_MAX_TALK || 4),        // replies per thread in all, answering people who reply to the bot
  budget: Number(env.BOT_BUDGET || 400),         // $ a month (UTC) for every model call; the looks wait when it runs short
  minConfidence: env.BOT_MIN_CONFIDENCE || 'medium',
  archive: env.BOT_ARCHIVE !== '0',
  state: path.resolve(HERE, env.BOT_STATE || 'state'),
  repo: env.BOT_REPO || defaultSource(HERE),
  branch: env.BOT_BRANCH || 'main',
  liveUrl: env.BOT_LIVE_URL || 'https://nfsu2.xyz/game.json', liveFile: env.BOT_LIVE_FILE || '/live/nfsu2.json',
};
if (!CFG.token) { console.error('DISCORD_TOKEN is not set (README.md)'); process.exit(2); }

const IMAGE_TYPES = ['image/png', 'image/jpeg', 'image/gif', 'image/webp'];
const MAX_TEXT = 10 << 20, MAX_IMAGE = 10 << 20, MAX_IMAGES = 4;   // pictures are shrunk for the model (picture())
const SAVES_ZIP = /^nfsu2-saves-.*\.zip$/i;      // Saves, Download backup: not for the model
const GIST_LOGS = 4;                             // the newest logs of a thread that get a gist (logs.mjs) in its transcript
const LOG_FILE = /\.(log|txt)$/i;                // F9, Download log; a long paste comes as message.txt
const hasLog = m => [...m.attachments.values()].some(a => LOG_FILE.test(a.name));
/** a message that mentions the bot or replies to one of its messages (also with the reply's ping turned off) */
const toBot = m => m.mentions.repliedUser?.id === client.user.id ||
  m.mentions.has(client.user, { ignoreEveryone: true, ignoreRoles: true, ignoreRepliedUser: true });
/** a message that only thanks or says okay, in the server's languages: nothing for a look to answer */
const THANKS = /^(?:[\s!.,:;()\-\p{Extended_Pictographic}\uFE0F]|ok(?:ay)?|thanks?|thank you|thx|ty|tysm|cheers|got it|will do|i'?ll try(?: it)?|a lot|so much|very much|obrigad[oa]|valeu|vlw|mesmo|gracias|merci|beaucoup|danke|спасибо|dzięki|dziękuję|teşekkürler|grazie|bedankt)+$/iu;
const thanks = m => !m.attachments.size && THANKS.test(m.content);
const MAX_WAIT_MS = 10 * 60_000;                 // a thread that keeps growing is looked at after 10 minutes anyway
const STARTUP_MS = 48 * 3600_000;                // at start, the forums' posts of the last 48 hours, nothing older
const RELEASE_CHECK_MS = 5 * 60_000;             // how often the bot checks whether a new version went live
const FIX_DAYS = 14, FIX_MAX = 15;               // a release is checked against threads of the last 14 days, 15 looks at most
const FIX_EFFORT = 'low', FIX_TURNS = 8;          // a fix check reads one commit against one thread: a short look
const LINKED_MAX = 5;                            // threads linked to a fixed one that get its 'fix live' look too, a release
const FEATURE_MAX = 5;                           // a release is checked against every open suggestion, 5 looks at most
const SUGGESTIONS = /suggest/i;                  // the suggestion forums, by name
const SUGGESTION_INDEX = 500;                    // a release reads the newest 500 archived threads of each of them
const SUGGESTION_DAYS = 365;                     // suggestions are kept in threads.json this long, other threads 30 days
const VOTES_MS = 3 * 3600_000;                   // how often the suggestions' votes (reactions, messages, people) are counted
const BUMP_QUIET_MS = 12 * 3600_000;             // the poster writing after this long a silence is a bump: they wait
const REPEAT = 0.5;                              // a reply whose word pairs the bot's earlier messages hold this much of
const MAX_ASKS = 2;                              // the bot's asks in a thread; a third is not posted
const OTHER_DAYS = 7, OTHER_MAX = 5;             // the poster's other threads a look is told of: active this recently, at most
const NOTES_KEEP = 6, NOTE_MAX = 2000;           // a thread's notes for the developer kept (the newest), and each one's length
const FLAGGED_DAYS = 14, FLAGGED_MAX = 25;       // the flagged threads a look is told of (flaggedList)

fs.mkdirSync(CFG.state, { recursive: true });
const STATE_FILE = path.join(CFG.state, 'threads.json');
const VERDICTS = path.join(CFG.state, 'verdicts.jsonl');
const LIVE_FILE = path.join(CFG.state, 'live.json');      // the live version the bot saw last
/** per thread: { title, forum, guild, owner, created, active, status: new|seen|none|asked|answered|staff|failed, replies,
 *  summary, category, snippet (its first post, for threads the bot only indexed), closed (archived, locked or deleted:
 *  not one to point people to), tops (where its logs crash or freeze, sigs.mjs), asks (the bot's asks in it),
 *  lastReply (the start of the bot's last reply), notes ([{ at, why, flag, text }]: the looks' notes for the developer,
 *  the newest NOTES_KEEP), flagged (when a look last said a person should look: escalate, or a handoff), sameAs (the
 *  first thread of the same problem, as a look said) and links (every one it said), crash (the tops of the crash or
 *  freeze it was opened for, sigs.mjs crashOf), fix ({ commit, subject, added, part, at }: what went live for it, as
 *  the bot told the thread), solved (when the poster said it works now, tags.mjs); from before 2026-11-05, when reports went to a staff channel: staffMsg, staffThread,
 *  report (on a report's first thread: { title, threads, parts: the notes, log, fix, ... }) and reportOf (on the
 *  later ones); for a suggestion: votes ({ emoji: count } on its first post),
 *  messages and members (Discord's counts for the thread; members stop at 50), counted (when) } */
let threads = {};
try { threads = JSON.parse(fs.readFileSync(STATE_FILE, 'utf8')); } catch { /* first start */ }
function save() {
  const cut = Date.now() - 30 * 86400_000, wishCut = Date.now() - SUGGESTION_DAYS * 86400_000;
  for (const [id, t] of Object.entries(threads)) if (Date.parse(t.created) < (isSuggestion(t) ? wishCut : cut)) delete threads[id];
  fs.writeFileSync(STATE_FILE + '.tmp', JSON.stringify(threads, null, 1));
  fs.renameSync(STATE_FILE + '.tmp', STATE_FILE);
}

const archive = CFG.archive ? new Archive(path.join(CFG.state, 'archive')) : null;
const repo = new Repo({ source: CFG.repo, branch: CFG.branch, dir: path.join(CFG.state, 'repo'), liveUrl: CFG.liveUrl, liveFile: CFG.liveFile,
  releasesFile: path.join(CFG.state, 'releases.json') });
const spend = new Spend(path.join(CFG.state, 'spend.json'), CFG.budget);
const anthropic = metered(newClient(), c => spend.add(c));
const client = new Client({
  intents: [GatewayIntentBits.Guilds, GatewayIntentBits.GuildMessages, GatewayIntentBits.MessageContent],
  partials: [Partials.Channel],
});

const isForum = ch => ch?.type === ChannelType.GuildForum || ch?.type === ChannelType.GuildMedia;
/** a known thread that suggests something: in a suggestion forum, or one the look called a suggestion */
function isSuggestion(t) { return t.category === 'suggestion' || SUGGESTIONS.test(t.forum ?? ''); }
function watched(thread) {
  return thread?.isThread() && isForum(thread.parent) && thread.parentId !== CFG.staffChannel &&
    (!CFG.forums.length || CFG.forums.includes(thread.parentId));
}
function isStaff(member, channel) {
  if (!member) return false;
  if (member.id === member.guild.ownerId || CFG.staff.includes(member.id)) return true;
  if (member.roles?.cache?.some(r => CFG.staff.includes(r.id))) return true;
  return !!channel && member.permissionsIn(channel).has(PermissionFlagsBits.ManageThreads);
}

const translator = new Translator({ client, anthropic, state: CFG.state });   // Apps, Translate on a message
// the FAQ (faq.mjs): /faq posts it; its text is faq.md in the clone, like system.md, so a push updates the posted message
const faq = new Faq({ client, anthropic, state: CFG.state, readFile: rel => repo.file(rel),
  read: async () => (await repo.file('discord-bot/faq.md')) || fs.readFileSync(path.join(HERE, 'faq.md'), 'utf8') });
// forum tags (tags.mjs); the poster saying it works now is kept on the thread, for the archive's reader
const tags = new Tags({ client, anthropic, isStaff, dryRun: CFG.dryRun,
  onSolved: thread => { if (threads[thread.id]) { threads[thread.id].solved = new Date().toISOString(); save(); } } });

// the general channels (general.mjs): pointers to the forums, and one-line fixes
const general = new General({
  client, anthropic, isStaff, dryRun: CFG.dryRun, state: CFG.state,
  releasedAt: () => repo.releases.at(-1)?.at ?? 0, download,
  bugForum: guild => guild?.channels.cache.find(ch => isForum(ch) && /bug/i.test(ch.name)) ?? null,
  forumThreads: () => Object.entries(threads)
    .filter(([, t]) => !t.closed && !isSuggestion(t))
    .sort(byActivity).slice(0, 40)
    .map(([id, t]) => ({ id, owner: t.owner, line: `${id} | ${t.title} | ${(t.summary || t.snippet || '').slice(0, 120)}` })),
});
// #help (help.mjs): the FAQ's answer to a question it settles, at once
const help = new Help({ client, anthropic, faq, general, isStaff, dryRun: CFG.dryRun, state: CFG.state });

// ---------------------------------------------------------------------------------------------- when to look --
const timers = new Map();                        // thread id -> { timer, first }
const queue = [];
let busy = false;
function schedule(threadId, why) {
  const now = Date.now();
  const t = timers.get(threadId);
  if (t) clearTimeout(t.timer);
  if (why === 'asked by staff') {                // staff asked: no wait, and ahead of the other looks
    timers.delete(threadId);
    console.log(`[bot] ${threadId} "${threads[threadId]?.title}": asked by staff, looking now`);
    return enqueue(threadId, why, undefined, true);
  }
  const first = t?.first ?? now;
  const delay = Math.max(0, Math.min(CFG.waitS * 1000, first + MAX_WAIT_MS - now));
  if (!t) console.log(`[bot] ${threadId} "${threads[threadId]?.title}": ${why}, a look in ${Math.round(delay / 1000)} s unless more comes`);
  timers.set(threadId, { first, why, timer: setTimeout(() => { timers.delete(threadId); enqueue(threadId, why); }, delay) });
}
function enqueue(threadId, why, extra, first = false) {
  const queued = queue.findIndex(q => q.threadId === threadId);
  if (queued >= 0 && first) queue.splice(queued, 1);
  if (queued < 0 || first) queue[first ? 'unshift' : 'push']({ threadId, why, extra });
  pump();
}
async function pump() {
  if (busy) return;
  busy = true;
  while (queue.length) {
    const job = queue.shift();
    try { await look(job.threadId, job.why, job.extra); } catch (e) { console.error(`[bot] ${job.threadId}: ${e.stack || e}`); }
  }
  busy = false;
}

// ---------------------------------------------------------------------------------------------------- a look --
async function look(threadId, why, extra) {
  const thread = await client.channels.fetch(threadId).catch(() => null);
  const st = threads[threadId];
  const say = s => console.log(`[look] ${threadId} ${s}`);
  if (!thread || !st || !watched(thread)) return say('skipped: the thread is gone or not in a watched forum');
  const fix = why === 'fix live' || why === 'implemented';      // a release's look: a fix, or a suggestion it adds
  if (thread.locked || (thread.archived && !fix)) return say('skipped: closed or locked');
  st.guild ??= thread.guildId;
  const byStaff = why === 'asked by staff', bumped = why === 'bump', logged = why === 'log';
  // someone answering the bot: the poster bringing what it asked for, or anyone replying to it
  const talk = why === 'reply' || why === 'follow-up';
  // looks that go ahead where staff wrote and past the reply cap; replies to the bot up to their own cap
  const anyway = byStaff || fix || bumped || logged || talk;
  if (fix && st.fixed) return say(`skipped: already told that ${why === 'implemented' ? 'it was added' : 'a fix went live'}`);
  if (fix && extra?.part && st.partly) return say('skipped: already told it about a step toward it');
  if (why === 'implemented' && Tags.decided(thread)) return say('skipped: the suggestion is Complete or Denied already');
  // the bot stepped back for a person: only staff bring it back (a fix notice still goes out)
  if (st.handoff && !byStaff && !fix) return say('skipped: the bot left the thread to staff');
  if (talk && st.replies >= CFG.maxTalk) {
    // the bot is out of replies here and they still talk to it: flagged once with what they wrote, so the thread shows
    // among the flagged ones in the archive (a Mac poster answered the bot's question and pinged it twice, unanswered)
    if (!st.capped) {
      const last = [...(await thread.messages.fetch({ limit: 5 }).catch(() => new Map())).values()]
        .find(m => m.author.id !== client.user.id && !m.author.bot);
      st.capped = new Date().toISOString();
      keep(st, threadId, { escalate: true, notes: `Past the bot's ${st.replies} replies, ${last?.author.id === thread.ownerId ? 'the poster' : 'a member'} ` +
        `still writes to it: "${(last?.content ?? '').replace(/\s+/g, ' ').slice(0, 400)}"` }, why, []);
      save();
    }
    return say(`skipped: ${st.replies} replies already, the most for answering replies${st.capped ? ' (flagged)' : ''}`);
  }
  if (!anyway && st.status === 'staff') return say('skipped: staff are in the thread');
  if (!anyway && st.replies >= CFG.maxReplies) return say(`skipped: ${st.replies} replies already`);
  // staff asked, or a release's notice (a few cents each, at most FIX_MAX + FEATURE_MAX a release): a look past the
  // budget too.  Someone talking to the bot (the log or the answer it asked for, a reply to it, a bump) goes ahead of
  // the clock, up to a guard (spend.underCap), and what it spends over the day's share comes out of the days after:
  // on 5 November, 41 of them were dropped while the day's share was spent.  A new post waits for none: past what the
  // day may have spent by now (spend.mjs), it is skipped
  if (!byStaff && !fix && (bumped || logged || talk) && !spend.underCap()) {
    return say(`skipped: $${spend.today.toFixed(2)} spent today, the most a day may spend on answers to the bot`);
  }
  // a new post past what the day may have spent by now: a quick pass with the cheapest model and no tools
  // (investigate.mjs BUDGET_PASS) instead of nothing, under the same guard as the answers to the bot
  const cheap = !byStaff && !fix && !(bumped || logged || talk) && !spend.allows();
  if (cheap && (why !== 'new post' || !spend.underCap())) {
    return say(`skipped: $${spend.today.toFixed(2)} spent today, $${spend.allowance().toFixed(2)} of today's $${spend.share().toFixed(2)} allowed by now`);
  }
  if (cheap) say(`$${spend.today.toFixed(2)} spent today, $${spend.allowance().toFixed(2)} allowed by now: a quick pass only`);

  const msgs = [...(await thread.messages.fetch({ limit: 100 })).values()].reverse();
  const starter = await thread.fetchStarterMessage().catch(() => null);
  if (starter && !msgs.some(m => m.id === starter.id)) msgs.unshift(starter);
  if (!anyway && msgs.some(m => m.author.id !== client.user.id && isStaff(m.member, thread))) {
    st.status = 'staff'; save();
    return say('skipped: staff are in the thread');
  }
  const bump = bumped && openBump(msgs.filter(m => !m.system), thread.ownerId);
  if (bumped && (!bump || st.bump === bump.id)) return say('skipped: the bump was answered already');
  if (bumped && st.solved && Date.parse(st.solved) >= bump.createdTimestamp) return say('skipped: the poster says it works now');
  if (logged && sameLog(msgs, thread)) return say('skipped: the same log was posted before');
  // staff said it themselves ("fix released") once the release was out: no notice on top
  const releasedAt = fix && repo.releases.find(r => r.rev === extra?.rev)?.at;
  if (releasedAt && msgs.some(m => m.author.id !== client.user.id && isStaff(m.member, thread) && m.createdTimestamp >= releasedAt - 60_000))
    return say('skipped: staff wrote in the thread since the release');

  const workDir = path.join(CFG.state, 'work', threadId);
  fs.rmSync(workDir, { recursive: true, force: true }); fs.mkdirSync(workDir, { recursive: true });
  const done = () => fs.rmSync(workDir, { recursive: true, force: true });
  const { text, images } = await transcript(thread, msgs.filter(m => !m.system), workDir, why, extra, standingOf(threadId, st, msgs));
  say(`"${st.title}" (${why}): ${msgs.length} message(s), ${fs.readdirSync(workDir).length} file(s), ${images.length} picture(s)` +
    `${images.length ? ` (${images.reduce((s, p) => s + (p.tokens ?? 0), 0)} tokens)` : ''}`);

  const started = Date.now();
  // staff are waiting for this one: "typing…" in the thread until the reply is there (Discord shows it ~10 s a call)
  const typing = byStaff && setInterval(() => thread.sendTyping().catch(() => {}), 8000);
  if (typing) thread.sendTyping().catch(() => {});
  const tops = folderTops(workDir);
  if (tops.length) st.tops = tops; else delete st.tops;
  const context = { repo, guild: thread.guildId, thread: text, images, threads: recentThreads(threadId), reports: flaggedList(threadId),
    places: placesOf(tops, threadId) };
  let res = null, first = null;
  if (cheap) {
    first = await firstPass({ client: anthropic, ...context, budget: true }).catch(e => { say(`quick pass failed: ${e.message}`); return null; });
    if (!first) return done();
    say(`quick pass: ${first.verdict?.action ?? 'no verdict'} (${first.verdict?.confidence ?? '-'})${first.cost != null ? ` ($${first.cost.toFixed(4)})` : ''}`);
    res = { verdict: first.verdict, turns: 1, cost: first.cost, usage: first.usage, log: ['quick pass only: the day\'s full looks were spent'] };
  }
  // someone answering the bot: a quick pass first (investigate.mjs replyPass), no full look for "thanks, works now"
  if (talk) {
    const pass = await replyPass({ client: anthropic, thread: text }).catch(e => { say(`reply pass failed: ${e.message}`); return null; });
    if (pass) say(`reply pass: ${pass.answer ? 'a full look' : 'nothing to answer, no full look'} (${pass.why})` +
      `${pass.cost != null ? ` ($${pass.cost.toFixed(4)})` : ''}`);
    if (pass && !pass.answer) {
      fs.appendFileSync(VERDICTS, JSON.stringify({ at: new Date().toISOString(), thread: threadId, title: st.title, why,
        outcome: `reply pass: nothing to answer (${pass.why})`, post: null, verdict: null, turns: 1, cost: pass.cost, usage: pass.usage }) + '\n');
      return done();
    }
  }
  // a new suggestion: a quick pass first (investigate.mjs firstPass), and a full look only when it sees something to say
  if (!res && why === 'new post' && SUGGESTIONS.test(st.forum ?? '')) {
    first = await firstPass({ client: anthropic, ...context }).catch(e => { say(`first pass failed: ${e.message}`); return null; });
    if (first) say(`first pass: ${first.verdict?.action ?? 'no verdict'}${first.cost != null ? ` ($${first.cost.toFixed(4)})` : ''}, ` +
      `${first.quiet ? 'nothing to say, no full look' : 'a full look'}`);
    if (first?.quiet) res = { verdict: first.verdict, turns: 1, cost: first.cost, usage: first.usage, log: ['first pass only'] };
  }
  if (!res) try {
    res = await investigate({
      client: anthropic, model: CFG.model, effort: fix ? FIX_EFFORT : CFG.effort, maxTurns: fix ? FIX_TURNS : undefined, ...context,
      archive, workDir, threadIndex: () => threadIndex(threadId), check: v => badLink(v.reply ?? '', threadId), step: say,
    });
    if (first) res.cost = res.cost == null || first.cost == null ? null : res.cost + first.cost;
  } catch (e) {
    if (!byStaff) { clearInterval(typing); throw e; }
    res = { verdict: null, turns: 0, cost: 0, log: [`failed: ${e.message}`] };
    say(`failed: ${e.message}`);
  }
  const v = res.verdict;
  // a quick pass flags nothing and steps back from nothing (a full look decides that), and its link to an earlier
  // thread counts only in a reply that sends the poster there
  if (cheap && v) Object.assign(v, { escalate: false, handoff: false,
    duplicate_of: v.duplicate_of && v.action !== 'none' && (v.reply ?? '').includes(v.duplicate_of) ? v.duplicate_of : '' });
  // a poster who brought what the bot asked for, or who bumped, always hears back
  let { post, why: outcome } = fix && v?.action !== 'answer' ? { post: null, why: v?.action ?? 'no verdict' }
    : vet(v, { minConfidence: fix || cheap ? 'high' : byStaff || bumped || why === 'follow-up' || v?.handoff ? 'low' : CFG.minConfidence });
  // a reply that says again what the bot said in the thread before: the rules ask for none, this catches the near copies
  const said = msgs.filter(m => m.author.id === client.user.id).map(m => m.content).join('\n');
  const repeat = post && !byStaff && !fix && said ? overlap(post, said) : 0;
  if (repeat >= REPEAT) { post = null; outcome = `a repeat of an earlier reply (${repeat.toFixed(2)})`; }
  // a reply in a script that nobody in the thread writes in (Russian to an English post with a Russian page in its
  // pictures): the reader's language is in their words (system.md "Write"), never in a picture or a name
  const foreign = post ? scriptsOf(post.replace(/`[^`]*`|https?:\/\/\S+/g, ''))
    .filter(k => !scriptsOf([thread.name, ...msgs.filter(m => m.author.id !== client.user.id).map(m => m.content)].join('\n')).includes(k)) : [];
  if (foreign.length) { post = null; outcome = `a reply in ${foreign.join(', ')}, which nobody in the thread writes`; }
  // a quick pass can't be sent back to write it again (investigate's check): a bad link drops its reply
  const linkWrong = cheap && post && badLink(post, threadId);
  if (linkWrong) { post = null; outcome = `a quick pass's reply with a bad link: ${linkWrong}`; }
  // the bot asks twice in a thread at most (system.md "Decide"); the look is told how often it did (standingOf)
  if (post && v.action === 'ask' && !byStaff && (st.asks ?? 0) >= MAX_ASKS) { post = null; outcome = `a third ask (${st.asks} before)`; }
  // stepping back is silent: no last word, never another round of the argument (nothing is passed on to anyone)
  if (post && v.handoff && !byStaff) { post = null; outcome = 'a handoff, the bot steps out without a word'; }
  if (byStaff && !post) {                        // staff asked: always a reply, at least what the look found
    const text = v ? `${v.summary}` : 'I could not finish a look at this thread just now.';
    post = vet({ ...(v ?? {}), action: 'answer', confidence: 'high', reply: text }, {}).post;
    outcome = `${outcome}, replying for staff`;
  }
  const posted = post && (!CFG.dryRun || byStaff);
  const secs = ((Date.now() - started) / 1000).toFixed(0);
  say(`done: ${outcome}${posted ? ', posted in the thread' : post ? ', dry run, not posted' : ', nothing posted'}; ` +
    `${res.turns} turns, ${secs} s${res.cost != null ? `, $${res.cost.toFixed(3)}` : ''}`);
  if (post) say(`reply: ${post.split('\n')[0].slice(0, 160)}`);

  if (posted) {
    // a reply to the poster's last post once the bot replied before; to the log it read, or to who talked to it, when
    // that brought it here
    const newest = [...msgs].reverse();
    const ref = (logged && newest.find(m => hasLog(m) && !isStaff(m.member, thread))) ||
      (talk && newest.find(m => m.author.id !== client.user.id && toBot(m))) ||
      (st.replies && newest.find(m => m.author.id === thread.ownerId));
    try {
      for (const [n, content] of blocks(post).entries()) {    // a long reply: several messages, split between its parts
        await thread.send({
          content, allowedMentions: { parse: [], repliedUser: false },
          ...(n === 0 && ref ? { reply: { messageReference: ref.id, failIfNotExists: false } } : {}),
        });
      }
    } finally { clearInterval(typing); }
    st.lastReply = post.replace(/\s+/g, ' ').slice(0, 200);   // for the looks at the poster's other threads
    if (!fix && v?.action === 'ask') st.asks = (st.asks ?? 0) + 1;
  }
  clearInterval(typing);
  if (fix) {                                     // a fix notice changes nothing else about the thread
    st.fixChecked = extra.rev;
    // a step toward a suggestion is told once, and leaves it open for the release that carries it out
    const part = !!extra.part;
    if (post) st[part ? 'partly' : 'fixed'] = extra.rev;
    if (posted) await thread.fetch(true).catch(() => {});      // the reply took it out of the archive
    const added = why === 'implemented';
    if (post && !part) await tags.add(thread, added ? 'complete' : 'fixLive').catch(e => console.error(`[tags] ${threadId}: ${e.message}`));
    // what went live for it, for the archive's reader (the discord-history MCP)
    if (post) st.fix = { commit: extra.commit, subject: extra.subject ?? '', added, part, at: new Date().toISOString() };
    save();
    fs.appendFileSync(VERDICTS, JSON.stringify({ at: new Date().toISOString(), thread: threadId, title: st.title, why, extra, outcome,
      dryRun: CFG.dryRun, post, verdict: v, turns: res.turns, cost: res.cost, usage: res.usage, tools: res.log }) + '\n');
    return done();
  }
  if (post) st.replies++;
  if (bump) st.bump = bump.id;
  if (v?.handoff && !byStaff) {                  // a person should take it: flagged, and the bot stays out
    st.handoff = new Date().toISOString();
    v.escalate = true;
    say('the bot leaves the thread to staff');
  }
  if (v) {
    Object.assign(st, { summary: v.summary, category: v.category });
    keep(st, threadId, v, why, tops);
  }
  if (!((bumped || logged || talk) && st.status === 'staff')) {   // a thread staff wrote in stays theirs
    st.status = !v ? 'failed' : post ? (v.action === 'ask' ? 'asked' : 'answered') : st.status === 'asked' ? 'asked-none' : 'none';
  }
  if (v) await tags.afterLook(thread, st, v, post).catch(e => console.error(`[tags] ${threadId}: ${e.message}`));
  save();
  fs.appendFileSync(VERDICTS, JSON.stringify({
    at: new Date().toISOString(), thread: threadId, title: st.title, why, outcome, dryRun: CFG.dryRun, post,
    verdict: v, turns: res.turns, cost: res.cost, usage: res.usage, tools: res.log,
  }) + '\n');
  done();
}

/** what a look found, kept on the thread for whoever reads the bot's archive (the discord-history MCP): the notes for
 *  the developer (the last NOTES_KEEP), the flag that a person should look (escalate), the threads it said are the
 *  same problem (sameAs: the first, links: all), and the crash or freeze the thread was opened for (the first look
 *  that names one; a later log of another bug never changes it) */
function keep(st, threadId, v, why, tops) {
  const at = new Date().toISOString();
  const notes = (v.notes ?? '').trim();
  if (notes) st.notes = [...(st.notes ?? []), { at, why, flag: !!v.escalate, text: notes.slice(0, NOTE_MAX) }].slice(-NOTES_KEEP);
  if (v.escalate) st.flagged = at;
  const dup = v.duplicate_of && v.duplicate_of !== threadId && threads[v.duplicate_of] ? v.duplicate_of : '';
  if (dup) {
    // the problem's first thread, while it is one to follow: a closed one or one whose fix went live is another problem's
    // (crashes in the name tags were joined to the fixed garage crash of 2 November through a thread that had both)
    const first = threads[dup].sameAs, f = first && threads[first];
    const root = first && first !== threadId && f && !f.closed && !f.fix ? first : dup;
    st.sameAs ??= root;
    if (!(st.links ??= []).includes(root)) st.links.push(root);
  }
  if (!st.crash?.length) st.crash = crashOf(v.crash, tops);
}

/** what a look knows of the thread besides its posts: the thread the bot linked it to, how often the bot replied and asked in it,
 *  and the poster's other threads with the bot's last reply in each (the same poster's two threads got opposite
 *  answers in one minute, and "Frame rate 30 helps" turned into "can't help" and back across two of them) */
function standingOf(threadId, st, msgs) {
  const out = [];
  const same = st.sameAs && threads[st.sameAs];
  if (same) out.push(`The bot said before that this thread is the same problem as ${st.sameAs}: ${(same.summary ?? same.title ?? '').slice(0, 200)}`);
  // a long reply goes out as several messages: one reply
  const replies = msgs.filter((m, i) => m.author.id === client.user.id && msgs[i - 1]?.author.id !== client.user.id).length;
  if (replies) out.push(`The bot replied ${replies} time(s) in this thread${st.asks ? `; it asked for more ${st.asks} time(s)` : ''}.`);
  const since = new Date(Date.now() - OTHER_DAYS * 86400_000).toISOString();
  const others = st.owner ? Object.entries(threads)
    .filter(([id, t]) => id !== threadId && t.owner === st.owner && (t.active ?? t.created) >= since).sort(byActivity).slice(0, OTHER_MAX) : [];
  if (others.length) out.push(`The poster's other threads (the newest first):\n${others.map(e =>
    `${threadLine(e, 160, false)}${e[1].lastReply ? `\n  The bot's last reply there: ${e[1].lastReply}` : ''}`).join('\n')}`);
  return out;
}

/** the thread as the model reads it: text attachments saved to workDir (whatever their name: a chrome://gpu report
 *  comes as .txt.phps, a long paste as message.txt), images inlined */
async function transcript(thread, msgs, workDir, why, extra, facts = []) {
  const applied = (thread.appliedTags ?? []).map(id => thread.parent.availableTags?.find(t => t.id === id)?.name).filter(Boolean);
  const topics = Tags.of(thread.parent).topics.map(t => t.name);
  const lines = [`Forum: #${thread.parent.name}`, `Title: ${thread.name}`, `Tags: ${applied.join(', ') || 'none'}`,
    ...(topics.length ? [`Tags you may add: ${topics.join(', ')}`] : []),
    `Why the bot looks now: ${WHY[why] ?? why}`, ...(extra?.text ? [extra.text] : []), ...facts, ''];
  const head = lines.length - 1, readable = [];  // the files the tools read, named in the header
  const images = [];
  // more pictures than the model may see: the newest (the oldest four once hid the screenshot a reply was about)
  const pics = msgs.flatMap(m => [...m.attachments.values()])
    .filter(a => IMAGE_TYPES.includes((a.contentType ?? '').split(';')[0]) && a.size <= MAX_IMAGE);
  const seen = new Set(pics.slice(-MAX_IMAGES).map(a => a.id));
  const gists = new Set(msgs.flatMap(m => [...m.attachments.values()]).filter(a => LOG_FILE.test(a.name)).slice(-GIST_LOGS).map(a => a.id));
  let n = 0;
  const who = m => m.author.id === client.user.id ? 'this bot' : m.author.id === thread.ownerId ? 'the poster'
    : isStaff(m.member, thread) ? 'staff' : 'another member';
  const time = m => m.createdAt.toISOString().slice(0, 16).replace('T', ' ');
  for (const m of msgs) {
    const to = m.reference?.messageId && msgs.find(x => x.id === m.reference.messageId);
    const re = to ? `, in reply to ${to.member?.displayName ?? to.author.username} (${who(to)}) at ${time(to)}` : '';
    lines.push(`[${time(m)} UTC] ${m.member?.displayName ?? m.author.username} (${who(m)}${re}):`);
    if (m.content) lines.push(m.content);
    for (const a of m.attachments.values()) {
      const kb = `${Math.round(a.size / 1024)} KB`;
      const type = (a.contentType ?? '').split(';')[0];
      const name = `${++n}-${a.name.replace(/[^\w.-]+/g, '_')}`;
      if (IMAGE_TYPES.includes(type) && a.size <= MAX_IMAGE && seen.has(a.id)) {
        const data = await download(a.url), pic = data && await picture(data);
        if (pic) {
          images.push(pic);
          lines.push(`  [picture ${images.length}: ${a.name}]`);
          continue;
        }
      }
      if (SAVES_ZIP.test(a.name)) { lines.push(`  [saves backup: ${a.name}, ${kb} (a zip, not readable here)]`); continue; }
      if (!/^(image|video|audio)\//.test(type) && !/\.(zip|7z|rar|mp4|mov|webm)$/i.test(a.name) && a.size <= MAX_TEXT) {
        const data = await download(a.url);
        if (data && !data.subarray(0, 8192).includes(0)) {
          fs.writeFileSync(path.join(workDir, name), data);
          lines.push(`  [attachment: attachments/${name}, ${kb}]`);
          readable.push(`attachments/${name} (${kb})`);
          // a page log: its gist under it (logs.mjs), for the thread's newest GIST_LOGS logs
          const gist = gists.has(a.id) ? logGist(data.toString('utf8')) : [];
          if (gist.length) lines.push(`  [its gist, made by the bot's code from the whole log:]`, ...gist.map(l => `    ${l}`));
          continue;
        }
      }
      lines.push(`  [attachment not opened: ${a.name}, ${type || 'unknown type'}, ${kb}]`);
    }
    lines.push('');
  }
  lines.splice(head, 0, `Files you can read: ${readable.join(', ') || 'none, this thread has no text attachments'}`);
  return { text: lines.join('\n'), images };
}

const WHY = {
  'new post': 'a new post',
  'follow-up': 'the poster wrote again after the bot asked for information',
  'reply': 'someone replied to the bot or mentioned it, or the poster wrote right after its message (see "When someone replies to the bot")',
  'asked by staff': 'staff mentioned the bot and want its reply: answer or ask, never none; with no fix and nothing to ask, answer with a short account of what you found (what the log shows, whether it is known or fixed)',
  'fix live': 'a release just went live, and a commit in it may fix this thread\'s problem (see "When a fix goes live")',
  'implemented': 'a release just went live, and a commit in it may add what this suggestion asks for (see "When a suggestion is carried out")',
  'bump': 'the poster wrote again after the thread was quiet for half a day or more: they are waiting for an answer (see "When the poster bumps")',
  'log': 'someone posted a log (or another text file) in the thread (see "When a log is posted")',
};

/** the newest log in these messages (oldest first), not from staff, was posted before: every log file in it, by name and
 *  size, is in an earlier message (the same file sent again) */
function sameLog(msgs, thread) {
  const i = msgs.findLastIndex(m => hasLog(m) && !isStaff(m.member, thread));
  if (i < 0) return false;
  const key = a => `${a.name} ${a.size}`, before = new Set(msgs.slice(0, i).flatMap(m => [...m.attachments.values()].map(key)));
  return [...msgs[i].attachments.values()].filter(a => LOG_FILE.test(a.name)).every(a => before.has(key(a)));
}

/** the poster's newest bump in these messages (oldest first) that neither staff nor the bot answered since, or null.
 *  A bump: the poster writes after nobody wrote for BUMP_QUIET_MS ("any ideas?", or new details after a long wait) */
function openBump(msgs, ownerId) {
  let bump = null;
  for (let i = 1; i < msgs.length; i++) {
    const m = msgs[i];
    if (m.author.id === client.user.id || (m.author.id !== ownerId && isStaff(m.member, m.channel))) bump = null;
    else if (m.author.id === ownerId && m.createdTimestamp - msgs[i - 1].createdTimestamp >= BUMP_QUIET_MS) bump = m;
  }
  return bump;
}

// ---------------------------------------------------------------------------------------------- a release --
/** a new live version: the threads its commits may fix get a 'fix live' look; at most FIX_MAX of them */
async function checkRelease() {
  const rev = await repo.liveRev();
  if (!rev) return;
  let last = null;
  try { last = JSON.parse(fs.readFileSync(LIVE_FILE, 'utf8')).rev; } catch { /* first start */ }
  if (last === rev) return;
  // the new version is kept once this release is dealt with, so a check that fails is tried again next time
  const seen = () => fs.writeFileSync(LIVE_FILE, JSON.stringify({ rev, at: new Date().toISOString() }));
  if (!last) { seen(); return console.log(`[release] live is ${rev}`); }
  await repo.sync(true);
  if (!(await repo.has(last)) || !(await repo.has(rev))) { seen(); return console.log(`[release] live ${last} -> ${rev}: not both in the code copy, skipped`); }
  const commits = (await repo.git(['log', '--no-merges', '-n', '60', '--format=commit %h %s%n%b', `${last}..${rev}`])).trim();
  if (!commits) { seen(); return console.log(`[release] live ${last} -> ${rev}: no new commits (a rollback?)`); }
  const since = new Date(Date.now() - FIX_DAYS * 86400_000).toISOString();
  const open = t => !t.fixed && t.fixChecked !== rev;
  const reports = Object.entries(threads)
    .filter(([, t]) => t.created >= since && open(t) && !isSuggestion(t) && t.category !== 'question')
    .sort(byActivity).slice(0, 200);
  // a thread the bot never looked at (staff answered before its look) is only a title: its first post too
  for (const [id, t] of reports.filter(([, t]) => !t.summary && !t.snippet)) {   // one at a time: the rate limits
    const thread = await client.channels.fetch(id).catch(() => null);
    const m = await thread?.fetchStarterMessage().catch(() => null);
    if (m) t.snippet = m.content.replace(/\s+/g, ' ').slice(0, 200);
  }
  save();
  const candidates = reports
    .map(([id, t]) => ({ id, line: `${id} | #${t.forum} | ${t.created.slice(0, 10)} | ${t.category ?? '?'} | ${t.title} | ${t.summary || t.snippet || ''}` }));
  await indexSuggestions().catch(e => console.error(`[release] reading the suggestions: ${e.message}`));
  const wishes = Object.entries(threads).filter(([, t]) => isSuggestion(t) && open(t) && !t.decided)
    .sort(byActivity).slice(0, 400)
    .map(([id, t]) => ({ id, line: `${id} | #${t.forum} | ${t.created.slice(0, 10)} | ${t.title} | ${t.summary || t.snippet || ''}` }));
  const n = commits.split('\ncommit ').length;
  console.log(`[release] live ${last} -> ${rev}: ${n} commit(s), checking ${candidates.length} thread(s) and ${wishes.length} suggestion(s)`);
  const o = { client: anthropic, model: CFG.model, effort: CFG.effort, commits: commits.slice(0, 60_000) };
  const [fixes, features] = await Promise.all([matchFixes({ ...o, candidates }), matchFeatures({ ...o, candidates: wishes })]);
  if (fixes.error || features.error) return console.log(`[release] matching failed, again at the next check: ${fixes.error || features.error}`);
  seen();
  const list = ms => ms.length ? `: ${ms.map(m => `${m.thread} (${m.commit}${m.part ? ', a step' : ''})`).join(', ')}` : '';
  console.log(`[release] ${fixes.matches.length} thread(s) may be fixed${list(fixes.matches)}; ` +
    `${features.matches.length} suggestion(s) may be carried out or get a step toward them${list(features.matches)}`);
  // whole suggestions first; a step toward one only where the bot told none before
  const wished = features.matches.filter(m => !m.part || !threads[m.thread]?.partly).sort((a, b) => a.part - b.part);
  // the threads the bot linked to a fixed one as the same problem get the look too, after the shortlist's: a poster
  // the bot told "a fix comes with an update" heard nothing when it went live (6 November: split screen, the freeze)
  const matched = new Set(fixes.matches.map(m => m.thread)), rootOf = id => threads[id]?.sameAs ?? id;
  const linked = fixes.matches.flatMap(m => reports
    .filter(([id, t]) => !matched.has(id) && (rootOf(id) === rootOf(m.thread) || t.links?.includes(m.thread)))
    .map(([id]) => ({ thread: id, commit: m.commit, why: `the bot linked it to ${m.thread} as the same problem; that one: ${m.why}` })))
    .filter((m, i, all) => all.findIndex(x => x.thread === m.thread) === i).slice(0, LINKED_MAX);
  if (linked.length) console.log(`[release] ${linked.length} thread(s) linked to them: ${linked.map(m => m.thread).join(', ')}`);
  for (const [ms, max, why] of [[[...fixes.matches, ...linked], FIX_MAX, 'fix live'], [wished, FEATURE_MAX, 'implemented']]) {
    for (const m of ms.slice(0, max)) {
      const what = why === 'fix live' ? "this thread's problem"
        : m.part ? 'a step toward what this suggestion asks for, not all of it' : 'what this suggestion asks for';
      const body = (await repo.git(['show', '--no-patch', '--format=%h %s%n%n%b', m.commit]).catch(() => '')).trim().slice(0, 1500);
      console.log(`[release] ${m.thread}: ${m.why}`);
      enqueue(m.thread, why, { rev, commit: m.commit, subject: body.split('\n')[0].replace(/^\S+\s*/, ''), ...(m.part ? { part: true } : {}),
        text: `The commit that went live:\n${body || m.commit}\nWhy it may be ${what}: ${m.why}` });
    }
  }
}

/** the suggestion forums' threads, the archived ones too (the newest SUGGESTION_INDEX of each), into `threads` with
 *  their first posts and whether they are decided (Complete, Denied, locked): what a release may carry out.  With
 *  `votes`, every one's votes too (countVotes) */
async function indexSuggestions(votes = false) {
  const forums = client.channels.cache.filter(ch => isForum(ch) && SUGGESTIONS.test(ch.name) && (!CFG.forums.length || CFG.forums.includes(ch.id)));
  let fresh = 0, counted = 0;
  for (const forum of forums.values()) {
    const found = [...(await forum.threads.fetchActive()).threads.values()];
    for (let before, more = true; more && found.length < SUGGESTION_INDEX;) {
      const page = await forum.threads.fetchArchived({ type: 'public', before, limit: 100 });
      found.push(...page.threads.values());
      before = page.threads.last();
      more = page.hasMore && !!before;
    }
    for (const t of found) {
      if (!threads[t.id]) fresh++;
      const st = threads[t.id] ??= { title: t.name, forum: forum.name, guild: forum.guildId, created: t.createdAt.toISOString(), status: 'seen', replies: 0 };
      Object.assign(st, { title: t.name, decided: t.locked || Tags.decided(t) });
      if (votes) { counted += await countVotes(t, st); continue; }   // the first post comes with them
      if (st.snippet || st.summary) continue;
      const m = await t.fetchStarterMessage().catch(() => null);   // one at a time: discord.js waits out the rate limits
      if (m) st.snippet = m.content.replace(/\s+/g, ' ').slice(0, 200);
    }
  }
  save();
  if (fresh) console.log(`[${votes ? 'votes' : 'release'}] ${fresh} suggestion thread(s) read for the first time`);
  if (votes) console.log(`[votes] ${counted} suggestion(s) counted`);
}

/** a suggestion's votes, for the developer (not the model): the reactions on its first post, and Discord's count of
 *  its thread's messages and people.  The first post straight from the API: discord.js's cached copy keeps reactions
 *  that are gone (the bot has no reactions intent, and a post without reactions comes without the field).  1 if read */
async function countVotes(t, st) {
  const m = await client.rest.get(Routes.channelMessage(t.id, t.id)).catch(() => null);   // a forum post's id is its first post's
  st.messages = t.messageCount ?? st.messages;
  st.members = t.memberCount ?? st.members;
  if (!m) return 0;                                // the first post was deleted
  st.votes = Object.fromEntries((m.reactions ?? []).map(r => [r.emoji.name ?? r.emoji.id, r.count]));
  if (!st.snippet && !st.summary) st.snippet = (m.content ?? '').replace(/\s+/g, ' ').slice(0, 200);
  st.counted = new Date().toISOString();
  return 1;
}

async function download(url) {
  try {
    const r = await fetch(url, { signal: AbortSignal.timeout(30_000) });
    return r.ok ? Buffer.from(await r.arrayBuffer()) : null;
  } catch { return null; }
}

/** one known thread as the model sees it; its summary cut to `max` characters; with link false, without its link (the
 *  context of a look gives the pattern once: ~20 tokens a line less) */
function threadLine([id, t], max = Infinity, link = true) {
  const what = (t.summary || (t.snippet ? `first post: ${t.snippet}` : '')).slice(0, max);
  return `- ${id}${link ? ` https://discord.com/channels/${t.guild}/${id}` : ''} #${t.forum}, ${(t.active ?? t.created).slice(0, 10)} ` +
    `[${[t.category, t.status, (t.flagged || t.staffMsg) && 'flagged for the developer', t.closed && 'closed',
      t.solved && 'the poster says it works now', t.fix && !t.fix.part && !t.fix.added && 'its fix is live'].filter(Boolean).join(', ')}] ` +
    `${t.title}${what ? `: ${what}` : ''}`;
}
const byActivity = (a, b) => (b[1].active ?? b[1].created).localeCompare(a[1].active ?? a[1].created);
/** what is wrong with the forum threads a reply links, or '': the thread itself, one newer than it (the bot sends
 *  posters to the older thread, never away from it), or a closed one (6 of the 12 wrong replies of 6-7 November) */
function badLink(reply, threadId, known = threads) {
  const ids = [...reply.matchAll(/discord\.com\/channels\/\d+\/(\d+)|<#(\d+)>/g)].map(m => m[1] ?? m[2]);
  for (const id of new Set(ids)) {
    if (id === threadId) return `Your reply links this very thread (${id}): they are in it.`;
    const t = known[id];
    if (!t) continue;                            // a channel, or a thread the bot doesn't know: nothing to say about it
    if (BigInt(id) > BigInt(threadId)) return `Your reply links thread ${id}, which is newer than this one: point a newer thread to the older, never the other way.`;
    if (t.closed) return `Your reply links thread ${id}, which is closed (${t.title}).`;
  }
  return '';
}
/** the threads active last, for the context */
function recentThreads(except) {
  return Object.entries(threads).filter(([id]) => id !== except).sort(byActivity).slice(0, 30).map(e => threadLine(e, 160, false)).join('\n');
}
/** the open threads a look flagged for the developer in the last FLAGGED_DAYS (reported to the old staff channel, too),
 *  the latest first, with the threads it linked to each: the problems a look points posters to and links threads to */
function flaggedList(except) {
  const since = new Date(Date.now() - FLAGGED_DAYS * 86400_000).toISOString();
  const linked = id => Object.values(threads).filter(t => t.sameAs === id || t.reportOf === id).length;
  return Object.entries(threads)
    .filter(([id, t]) => id !== except && !t.closed && !t.sameAs && !t.reportOf && ((t.flagged ?? '') >= since || (t.staffMsg && t.created >= since)))
    .sort((a, b) => (b[1].flagged ?? b[1].created).localeCompare(a[1].flagged ?? a[1].created)).slice(0, FLAGGED_MAX)
    .map(([id, t]) => `- ${id} [${(t.flagged ?? t.created).slice(0, 10)}${linked(id) ? `, ${linked(id)} more thread(s)` : ''}` +
      `${t.fix && !t.fix.part ? `, ${t.fix.added ? 'added' : 'fix live'} ${t.fix.commit}` : ''}] ${(t.report?.title ?? t.summary ?? t.title).slice(0, 150)}`)
    .join('\n');
}
/** for a look: where this thread's logs crash or freeze, and the open threads opened for a crash or freeze in the same
 *  place (st.crash: the one each was opened for, never a later log's), the oldest first: the thread to point to */
function placesOf(tops, threadId) {
  if (!tops.length) return '';
  const same = Object.entries(threads)
    .filter(([id, t]) => id !== threadId && !t.closed && t.crash?.some(a => tops.some(b => samePlace(a, b))))
    .sort((a, b) => a[1].created.localeCompare(b[1].created)).slice(0, 5)
    .map(([id, t]) => `${id} (${t.created.slice(0, 10)}${t.fixed ? ', a fix went live for it' : ''}) ${t.title}`);
  return `Where this thread's logs crash or freeze (the functions at the top of the stack): ${tops.join('; ')}\n` +
    `Open threads opened for a crash or freeze in the same place, the oldest first: ${same.length ? same.join('; ') : 'none'}`;
}
/** every known thread, for the search_threads tool: [{ line, text }] */
function threadIndex(except) {
  return Object.entries(threads).filter(([id]) => id !== except).sort(byActivity)
    .map(e => ({ line: threadLine(e), text: `${e[1].title}\n${e[1].summary ?? ''}\n${e[1].snippet ?? ''}` }));
}

/** the forums' threads with posts in the last STARTUP_MS (titles, first posts), so the bot can point to them; the
 *  ones opened in that time that it never looked at get their look now, so a restart misses no post.  Nothing older */
async function indexForums() {
  const since = Date.now() - STARTUP_MS;
  const snowflakeTime = id => Number((BigInt(id) >> 22n) + 1420070400000n);
  const found = [], recent = [];
  for (const guild of client.guilds.cache.values()) {
    const active = await guild.channels.fetchActiveThreads().catch(() => null);
    // a known thread that is not active was archived (or deleted) while the bot was away
    if (active) for (const [id, st] of Object.entries(threads)) if (st.guild === guild.id) st.closed = !!(active.threads.get(id)?.locked ?? true);
    for (const t of active?.threads.values() ?? []) {
      if (!watched(t) || Math.max(t.createdTimestamp, t.lastMessageId ? snowflakeTime(t.lastMessageId) : 0) < since) continue;
      const fresh = !threads[t.id] && t.createdTimestamp >= since;
      const st = threads[t.id] ??= { title: t.name, forum: t.parent.name, guild: guild.id, created: t.createdAt.toISOString(),
        status: fresh ? 'new' : 'seen', replies: 0 };
      st.title = t.name; st.owner ??= t.ownerId;
      if (fresh) schedule(t.id, 'new post');
      else if (st.status !== 'new') recent.push(t);
      if (!st.snippet && !st.summary) found.push(t);
    }
  }
  save();
  console.log(`[bot] forums: ${found.length} threads with posts in the last ${STARTUP_MS / 3600_000} h read, ` +
    `${Object.values(threads).filter(t => t.status === 'new').length} waiting for a look`);
  for (const t of found) {                         // one at a time: discord.js waits out the rate limits
    const m = await t.fetchStarterMessage().catch(() => null);
    if (m) threads[t.id].snippet = m.content.replace(/\s+/g, ' ').slice(0, 200);
  }
  save();
  // bumps while the bot was away
  for (const t of recent) {
    const msgs = await t.messages.fetch({ limit: 25 }).catch(() => null);
    const bump = msgs && openBump([...msgs.values()].reverse().filter(m => !m.system), t.ownerId);
    if (bump && bump.createdTimestamp >= since && threads[t.id].bump !== bump.id) schedule(t.id, 'bump');
  }
}

// ------------------------------------------------------------------------------------------------- events --
client.on(Events.ClientReady, c => {
  console.log(`[bot] ${c.user.tag} on ${c.guilds.cache.size} server(s); ${CFG.dryRun ? 'dry run (replies logged, not posted), ' : ''}` +
    `${CFG.forums.length ? `forums ${CFG.forums.join(', ')}` : 'every forum'}, model ${CFG.model} (${CFG.effort}), ` +
    `$${spend.month.toFixed(2)} of $${CFG.budget} spent this month, $${spend.today.toFixed(2)} today ` +
    `($${spend.allowance().toFixed(2)} of $${spend.share().toFixed(2)} allowed by now)`);
  repo.sync(true).catch(e => console.error(`[repo] ${e.message}`));
  // the staff channel's messages saved before they were marked, by its name
  if (archive && CFG.staffChannel) c.channels.fetch(CFG.staffChannel).then(ch => { archive.staffName = ch?.name; }).catch(() => {});
  c.application.commands.set([TRANSLATE, FAQ]).catch(e => console.error(`[bot] the commands: ${e.message}`));
  let checking = null;                           // one check at a time: the first one reads every suggestion
  const release = () => checking ??= checkRelease().catch(e => console.error(`[release] ${e.message}`)).finally(() => { checking = null; });
  let counting = null;                           // the suggestions' votes, one count at a time
  const votes = () => counting ??= indexSuggestions(true).catch(e => console.error(`[votes] ${e.message}`)).finally(() => { counting = null; });
  indexForums().catch(e => console.error(`[bot] forum index: ${e.message}`)).finally(() => { release(); votes(); });
  setInterval(release, RELEASE_CHECK_MS);
  // faq.md pushed: the posted FAQ follows (the clone is fetched at most every 5 minutes)
  const faqs = () => repo.sync().then(() => faq.refresh()).catch(e => console.error(`[faq] ${e.message}`));
  faqs(); setInterval(faqs, RELEASE_CHECK_MS);
  setInterval(votes, VOTES_MS);
  setInterval(() => general.tick().catch(e => console.error(`[general] ${e.message}`)), 60_000);
  // posts that came in shortly before a restart still get their look
  for (const [id, t] of Object.entries(threads)) if (t.status === 'new' && Date.now() - Date.parse(t.created) < STARTUP_MS) schedule(id, 'new post');
});

/** an event handler that logs what goes wrong instead of taking the bot down */
const guard = (name, fn) => (...a) => { try { fn(...a); } catch (e) { console.error(`[bot] ${name}: ${e.stack || e}`); } };

client.on(Events.ThreadCreate, guard('new thread', (thread, created) => {
  if (!created || !watched(thread) || threads[thread.id]) return;
  const now = new Date().toISOString();
  threads[thread.id] = { title: thread.name, forum: thread.parent.name, guild: thread.guildId, owner: thread.ownerId, created: now, active: now,
    status: 'new', replies: 0 };
  save();
  schedule(thread.id, 'new post');
}));

client.on(Events.ThreadUpdate, guard('thread update', (old, thread) => {
  const st = threads[thread.id];
  const closed = !!(thread.archived || thread.locked);
  if (st && !!st.closed !== closed) { st.closed = closed; save(); }
}));
client.on(Events.ThreadDelete, guard('thread deleted', thread => {
  if (threads[thread.id] && !threads[thread.id].closed) { threads[thread.id].closed = true; save(); }
}));

client.on(Events.MessageCreate, guard('message', m => {
  if (m.system) return;                          // "changed the post title", pins, joins
  try { general.onMessage(m); } catch (e) { console.error(`[general] message: ${e.stack || e}`); }
  try { help.onMessage(m); } catch (e) { console.error(`[help] message: ${e.stack || e}`); }
  if (archive && m.guild) {
    archive.add({
      id: m.id, at: m.createdAt.toISOString(),
      channel: m.channel.isThread() ? m.channel.parent?.name : m.channel.name, thread: m.channel.isThread() ? m.channel.name : '',
      author: m.member?.displayName ?? m.author.username, text: m.content.slice(0, 4000), files: [...m.attachments.values()].map(a => a.name),
      // a private staff channel and its threads: a look may learn from them, never repeat them
      ...(CFG.staffChannel && (m.channelId === CFG.staffChannel || m.channel.parentId === CFG.staffChannel) ? { staff: true } : {}),
    }, [...m.attachments.values()].map(a => ({ url: a.url, name: a.name, size: a.size, type: a.contentType })));
  }
  const thread = m.channel;
  if (m.author.bot || !watched(thread)) return;
  // a thread from before the forum index (no posts in the hours it reads at start): known from now on.  A new one
  // waits for its ThreadCreate, which gives it its first look
  if (!threads[thread.id] && Date.now() - thread.createdTimestamp > 10 * 60_000) {
    threads[thread.id] = { title: thread.name, forum: thread.parent.name, guild: thread.guildId, created: thread.createdAt.toISOString(),
      status: 'seen', replies: 0 };
  }
  const st = threads[thread.id];
  if (!st) return;
  st.owner ??= thread.ownerId;
  st.active = m.createdAt.toISOString();
  tags.onMessage(m);                             // the poster saying it works now: Solved
  // staff ask by a mention by name; anyone else talks to the bot by a mention or a reply to its message
  const named = m.mentions.has(client.user, { ignoreEveryone: true, ignoreRoles: true, ignoreRepliedUser: true });
  if (isStaff(m.member, thread)) {
    if (named) { schedule(thread.id, 'asked by staff'); return; }
    if (st.status !== 'staff') console.log(`[bot] ${thread.id} "${st.title}": staff wrote, the bot stays out`);
    st.status = 'staff'; save();
    const t = timers.get(thread.id); if (t) { clearTimeout(t.timer); timers.delete(thread.id); }
    return;
  }
  const op = m.author.id === thread.ownerId;
  if (st.status === 'new') { if (op) schedule(thread.id, 'new post'); return; }
  // the poster still writing while a look waits ("here", the log, then a question): it waits for them too
  if (op && timers.has(thread.id)) { schedule(thread.id, timers.get(thread.id).why); return; }
  if (op && st.status === 'asked') { if (!thanks(m)) schedule(thread.id, 'follow-up'); return; }
  // a log, from the poster or anyone else: always a look, also past the reply cap and where staff wrote
  if (hasLog(m)) { schedule(thread.id, 'log'); return; }
  // someone talking to the bot: an answer, up to BOT_MAX_TALK replies in the thread, also where staff wrote
  if (!op) {
    if (thanks(m)) return;
    if (toBot(m)) { schedule(thread.id, 'reply'); return; }
    // right after the bot's message to them (it replied to one of theirs), with nobody between: an answer to it
    thread.messages.fetch({ before: m.id, limit: 1 }).then(before => {
      const prev = before.first();
      if (prev?.author.id === client.user.id && prev.mentions.repliedUser?.id === m.author.id) schedule(thread.id, 'reply');
    }).catch(e => console.error(`[bot] ${thread.id}: ${e.message}`));
    return;
  }
  // the poster after a quiet half day: a bump, also in a thread staff wrote in before and left.  Right after the
  // bot's message, with nobody between, they answer it, with Discord's reply or without ("that didn't work", "where is
  // that setting?"); a bare thanks gets no look
  thread.messages.fetch({ before: m.id, limit: 1 }).then(before => {
    const prev = before.first();
    if (prev && m.createdTimestamp - prev.createdTimestamp >= BUMP_QUIET_MS) schedule(thread.id, 'bump');
    else if ((toBot(m) || prev?.author.id === client.user.id) && !thanks(m)) schedule(thread.id, 'reply');
  }).catch(e => console.error(`[bot] ${thread.id}: ${e.message}`));
}));
client.on(Events.InteractionCreate, i => {
  if (i.isMessageContextMenuCommand() && i.commandName === TRANSLATE.name) {
    translator.handle(i).catch(e => console.error(`[translate] ${i.targetId}: ${e.stack || e}`));
  } else if (i.isChatInputCommand() && i.commandName === FAQ.name) {
    faq.command(i).catch(e => console.error(`[faq] /faq: ${e.stack || e}`));
  } else if (i.isButton() && i.customId.startsWith(FAQ_BUTTON)) {
    faq.click(i).catch(e => console.error(`[faq] ${i.customId}: ${e.stack || e}`));
  }
});
client.on(Events.Error, e => console.error(`[bot] discord: ${e.message}`));
process.on('unhandledRejection', e => console.error(`[bot] ${e?.stack || e}`));

client.login(CFG.token).catch(e => { console.error(`[bot] login: ${e.message}`); process.exit(1); });
