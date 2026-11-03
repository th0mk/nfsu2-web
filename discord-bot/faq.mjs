// The FAQ: one message (Components V2) in a read-only channel with the questions players ask most, as buttons under
// their topics; a click answers in a message only the one who clicked sees, in their Discord language.  The questions
// and answers are faq.md, read from the bot's clone like system.md, so a pushed change reaches the posted message
// within minutes: the bot edits it itself.  /faq (for those who may manage the server) posts it in the channel it is
// typed in, or brings the one there up to date.  An answer in another language costs one short call to the cheapest
// model the first time someone with that language asks, and is kept (faq-translations.json) until the answer changes.
import { ApplicationCommandType, ButtonBuilder, ButtonStyle, ContainerBuilder, InteractionContextType, MessageFlags,
  PermissionFlagsBits, SeparatorBuilder, SeparatorSpacingSize, TextDisplayBuilder } from 'discord.js';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

export const COMMAND = {
  name: 'faq', description: 'Post the frequently asked questions in this channel, or bring them up to date here',
  type: ApplicationCommandType.ChatInput, contexts: [InteractionContextType.Guild],
  defaultMemberPermissions: PermissionFlagsBits.ManageGuild,
};
export const PREFIX = 'faq:';                    // a question's button: faq:<its id>
const ACCENT = 0xc8f088;                         // the page's lime, sampled from the game's menus
const MAX_TEXT = 4000;                           // all the text of a Components V2 message
const MAX_COMPONENTS = 40;
const MODEL = 'claude-haiku-5-5';                // translating an answer: the cheapest model is enough
const GLOSSARY_MAX = 40;                         // the page's labels an answer names, with their translations
// Discord's languages that the page has a translation of: its labels, for the answers to name them as the page does.
// Discord has only Brazilian Portuguese; the page's Chinese is simplified (zh-CN; zh-TW gets no glossary)
const PAGE_LANGS = { pt: 'portuguese', es: 'spanish', ru: 'russian', tr: 'turkish', pl: 'polish', hu: 'hungarian',
  uk: 'ukrainian', fr: 'french', de: 'german', it: 'italian', nl: 'dutch', sv: 'swedish', da: 'danish', zh: 'chinese',
  ja: 'japanese', ko: 'korean', th: 'thai' };

const slug = s => s.toLowerCase().normalize('NFKD').replace(/[^\p{L}\p{N}]+/gu, '-').replace(/^-|-$/g, '').slice(0, 60);
const hashOf = s => crypto.createHash('sha1').update(s).digest('base64url').slice(0, 12);
/** the language an answer goes out in: Discord's locale without its region, but Chinese keeps it (zh-CN, zh-TW) */
export const langOf = locale => { const l = String(locale || 'en'); return /^zh/i.test(l) ? l : l.split('-')[0].toLowerCase(); };
const NAMES = new Intl.DisplayNames(['en'], { type: 'language' });
const nameOf = lang => { try { return lang === 'pt' ? 'Brazilian Portuguese' : NAMES.of(lang) ?? lang; } catch { return lang; } };
const escapeRe = s => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/** faq.md: "# Title", lines of intro, then "## Topic" groups of "### Question" with its answer below it.  A question's
 *  "label:" line names its button (else the question itself); its id is its label's slug, so an answer can change
 *  without the button changing */
export function parse(md) {
  const faq = { title: '', intro: [], topics: [] };
  let topic = null, q = null;
  for (const line of md.replace(/<!--[\s\S]*?-->/g, '').split(/\r?\n/)) {
    let m;
    if ((m = line.match(/^# (.+)/))) faq.title = m[1].trim();
    else if ((m = line.match(/^## (.+)/))) { faq.topics.push(topic = { name: m[1].trim(), questions: [] }); q = null; }
    else if ((m = line.match(/^### (.+)/))) {
      if (!topic) faq.topics.push(topic = { name: '', questions: [] });
      topic.questions.push(q = { q: m[1].trim(), label: '', a: [] });
    } else if (q && !q.a.length && (m = line.match(/^label:\s*(.+)/i))) q.label = m[1].trim();
    else if (q) q.a.push(line);
    else if (!topic) faq.intro.push(line);
  }
  faq.intro = faq.intro.join('\n').trim();
  for (const t of faq.topics) for (const x of t.questions) {
    x.a = x.a.join('\n').trim();
    x.label ||= x.q;
    x.id = slug(x.label);
  }
  return faq;
}

/** what keeps Discord from taking the message: too many parts, a question twice, a label too long, an empty answer */
export function check(faq) {
  const problems = [], seen = new Set(), all = faq.topics.flatMap(t => t.questions);
  if (!faq.title) problems.push('no "# Title"');
  if (!all.length) problems.push('no questions');
  for (const x of all) {
    if (seen.has(x.id)) problems.push(`"${x.label}" twice`);
    seen.add(x.id);
    if (x.label.length > 80) problems.push(`"${x.label.slice(0, 40)}…": a button's label takes 80 characters`);
    if (!x.a) problems.push(`"${x.q}" has no answer`);
    if (x.q.length + x.a.length + 10 > MAX_TEXT) problems.push(`"${x.q}": the answer is longer than a message takes`);
  }
  if (problems.length) return problems;
  let view;
  try { view = post(faq); } catch (e) { return [`Discord would refuse it: ${e.message}`]; }
  const { n, chars } = count(view);
  if (n > MAX_COMPONENTS) problems.push(`${n} parts, a message takes ${MAX_COMPONENTS}`);
  if (chars > MAX_TEXT) problems.push(`${chars} characters of text, a message takes ${MAX_TEXT}`);
  return problems;
}

/** every component in a message, nested ones too (Discord takes 40), and the characters of its text */
function count(components) {
  let n = 0, chars = 0;
  const walk = c => {
    n++; chars += c.content?.length ?? 0;
    for (const k of c.components ?? []) walk(k);
    if (c.accessory) walk(c.accessory);
  };
  for (const c of components) walk(c.toJSON?.() ?? c);
  return { n, chars };
}

const text = s => new TextDisplayBuilder().setContent(s);
const line = () => new SeparatorBuilder().setDivider(true).setSpacing(SeparatorSpacingSize.Small);

/** the posted message: the title, the intro, then each topic's name and its questions' buttons */
export function post(faq) {
  const box = new ContainerBuilder().setAccentColor(ACCENT)
    .addTextDisplayComponents(text([`# ${faq.title}`, faq.intro].filter(Boolean).join('\n')));
  for (const t of faq.topics) {
    box.addSeparatorComponents(line());
    if (t.name) box.addTextDisplayComponents(text(`### ${t.name}`));
    for (let i = 0; i < t.questions.length; i += 5) {   // an action row takes 5 buttons
      box.addActionRowComponents(row => row.addComponents(t.questions.slice(i, i + 5).map(x =>
        new ButtonBuilder().setCustomId(PREFIX + x.id).setLabel(x.label).setStyle(ButtonStyle.Secondary))));
    }
  }
  return [box];
}

/** one question's answer, for the one who asked; #channel-name as a link to the server's channel of that name */
export function answer(x, guild) {
  const linked = x.a.replace(/(^|[\s(])#([\p{L}\p{N}_-]+)/gu, (all, before, name) => {
    const ch = guild?.channels.cache.find(c => c.name === name);
    return ch ? `${before}<#${ch.id}>` : all;
  });
  return [new ContainerBuilder().setAccentColor(ACCENT).addTextDisplayComponents(text(`## ${x.q}\n${linked}`))];
}

const OUT = {
  type: 'object', additionalProperties: false, required: ['q', 'a'],
  properties: { q: { type: 'string' }, a: { type: 'string' } },
};
const SYSTEM = `You translate one answer from the FAQ of the Discord server of "Underground 2 Revived" (Need for Speed: Underground 2 in the browser, at nfsu2.xyz) into the language you are given. q is the question, a its answer.

- Keep the meaning and the length: nothing added, nothing left out. Plain and friendly, as a player would write it.
- Keep Discord's markdown as it is: **bold**, list dashes and numbers, line breaks.
- Keep as they are: \`inline code\` (addresses, codes, folders), keys (F9, T, R, Ctrl+F5, D-pad Down), channel names after #, names of browsers, companies, systems and games, and numbers.
- The page's buttons and settings: where a <glossary> lists the English words, use its translation exactly. The game's own menus and options that it does not list (Options, Gameplay, Save = Prompt, Career Status, Body Shop, Online mode) stay in English.`;

export class Faq {
  /** client: discord.js Client; anthropic: the metered API client; state: the state folder; read: async () => faq.md's
   *  text (the clone's, else the image's); readFile: async (path in the repository) => its text in the clone, or '' */
  constructor({ client, anthropic, state, read, readFile }) {
    Object.assign(this, { client, anthropic, read, readFile });
    this.file = path.join(state, 'faq.json');
    this.saved = { posts: [] };                  // [{ channel, message, hash }]: the messages the bot posted and what they show
    try { Object.assign(this.saved, JSON.parse(fs.readFileSync(this.file, 'utf8'))); } catch { /* none posted yet */ }
    this.trFile = path.join(state, 'faq-translations.json');
    this.tr = {};                                // language -> question id -> { hash, q, a }
    try { this.tr = JSON.parse(fs.readFileSync(this.trFile, 'utf8')); } catch { /* none yet */ }
    this.inflight = new Map();                   // `${language}:${id}` -> the call translating it now
    this.glossaries = new Map();                 // language -> [[English, translation]] of the page's labels
    this.faq = null; this.hash = null; this.problems = [];
  }

  save() {
    fs.writeFileSync(this.file + '.tmp', JSON.stringify(this.saved, null, 1));
    fs.renameSync(this.file + '.tmp', this.file);
  }

  saveTr() {
    const ids = new Set(this.faq?.topics.flatMap(t => t.questions.map(x => x.id)) ?? []);
    for (const byId of Object.values(this.tr)) for (const id of Object.keys(byId)) if (!ids.has(id)) delete byId[id];
    fs.writeFileSync(this.trFile + '.tmp', JSON.stringify(this.tr));
    fs.renameSync(this.trFile + '.tmp', this.trFile);
  }

  /** faq.md read again; a version Discord would refuse keeps the one before it (and the log says why) */
  async load() {
    const md = await this.read(), hash = hashOf(md);
    if (hash === this.hash) return;
    this.hash = hash;
    this.glossaries.clear();                     // a push may have changed the page's translations too
    const faq = parse(md), problems = check(faq);
    this.problems = problems;
    if (problems.length) { console.error(`[faq] faq.md not taken: ${problems.join('; ')}`); return; }
    this.faq = faq;
    this.view = hashOf(JSON.stringify(post(faq).map(c => c.toJSON())));
  }

  /** the posted messages brought up to date with faq.md; one the bot can no longer find is forgotten */
  async refresh() {
    await this.load();
    if (!this.faq) return;
    let changed = false;
    for (const p of [...this.saved.posts]) {
      if (p.hash === this.view) continue;
      try {
        const ch = await this.client.channels.fetch(p.channel);
        const m = await ch.messages.fetch(p.message);
        await m.edit({ components: post(this.faq), flags: MessageFlags.IsComponentsV2, allowedMentions: { parse: [] } });
        p.hash = this.view; changed = true;
        console.log(`[faq] updated in #${ch.name}`);
      } catch (e) {
        if (e.code === 10003 || e.code === 10008) {          // Unknown Channel, Unknown Message: deleted
          this.saved.posts = this.saved.posts.filter(x => x !== p); changed = true;
          console.log(`[faq] the message in ${p.channel} is gone: forgotten`);
        } else console.error(`[faq] updating ${p.channel}: ${e.message}`);
      }
    }
    if (changed) this.save();
  }

  /** /faq: the FAQ in this channel, a new message or the one already here brought up to date */
  async command(i) {
    await i.deferReply({ flags: MessageFlags.Ephemeral });
    const say = content => i.editReply({ content });
    await this.load();
    if (!this.faq) return say(`faq.md has a problem: ${this.problems.join('; ')}`);
    const ch = i.channel ?? await this.client.channels.fetch(i.channelId).catch(() => null);
    if (!ch?.send) return say('I can\'t see this channel: give me View Channel and Send Messages in its permissions.');
    const view = { components: post(this.faq), flags: MessageFlags.IsComponentsV2, allowedMentions: { parse: [] } };
    const old = this.saved.posts.find(p => p.channel === ch.id);
    if (old) {
      const m = await ch.messages.fetch(old.message).catch(() => null);
      if (m) {
        await m.edit(view);
        old.hash = this.view; this.save();
        return say('The FAQ in this channel is up to date.');
      }
      this.saved.posts = this.saved.posts.filter(p => p !== old);
    }
    try {
      const m = await ch.send(view);
      this.saved.posts.push({ channel: ch.id, message: m.id, hash: this.view });
      this.save();
      console.log(`[faq] posted in #${ch.name}`);
      return say('Posted. A change to faq.md shows here within minutes.');
    } catch (e) {
      console.error(`[faq] posting in #${ch.name}: ${e.message}`);
      return say(e.code === 50013 || e.code === 50001
        ? 'I can\'t post here: give me View Channel and Send Messages in this channel\'s permissions.'
        : `Could not post: ${e.message}`);
    }
  }

  /** a question's button: its answer, for this person only, in their Discord language */
  async click(i) {
    if (!this.faq) await this.load();
    const id = i.customId.slice(PREFIX.length);
    const x = this.faq?.topics.flatMap(t => t.questions).find(q => q.id === id);
    const view = y => ({ components: answer(y, i.guild), flags: MessageFlags.IsComponentsV2, allowedMentions: { parse: [] } });
    if (!x) return i.reply({ content: 'This question is gone: the list above is up to date.', flags: MessageFlags.Ephemeral });
    const lang = langOf(i.locale);
    if (lang === 'en' || !this.anthropic) return i.reply({ ...view(x), flags: MessageFlags.IsComponentsV2 | MessageFlags.Ephemeral });
    const terms = await this.terms(lang, x), hash = hashOf(JSON.stringify([x.q, x.a, terms]));
    const kept = this.tr[lang]?.[id];
    if (kept?.hash === hash) return i.reply({ ...view(kept), flags: MessageFlags.IsComponentsV2 | MessageFlags.Ephemeral });
    await i.deferReply({ flags: MessageFlags.Ephemeral });
    const key = `${lang}:${id}:${hash}`;
    let out = x;                                 // the English answer when the translation fails
    try {
      if (!this.inflight.has(key)) this.inflight.set(key, this.translate(x, lang, terms, hash).finally(() => this.inflight.delete(key)));
      out = await this.inflight.get(key);
    } catch (e) { console.error(`[faq] ${id} into ${lang}: ${e.message}`); }
    return i.editReply(view(out));
  }

  /** the page's labels that this answer names, with their translations into this language */
  async terms(lang, x) {
    if (!this.glossaries.has(lang)) {
      let map = {};
      const file = PAGE_LANGS[lang === 'zh-CN' ? 'zh' : lang];
      if (file && this.readFile) try { map = JSON.parse(await this.readFile(`web/src/app/lang/${file}.json`) || '{}'); } catch { /* none */ }
      // labels: capitalised, short, without markup or placeholders ("Download backup", not "the browser")
      this.glossaries.set(lang, Object.entries(map).filter(([k, v]) => typeof v === 'string' && /^\p{Lu}/u.test(k) && k.length <= 40 && !/[<{&]/.test(k)));
    }
    // the longest first, and what it covers left out for the shorter ones: "Turn on", not "Turn" too.  A label of
    // several words anywhere ("Download backup"); one word only where the text names a label: in bold, or in a menu
    // path ("Settings, Display, Frame rate"), so a sentence's "Right" or "Close" stays the model's to translate
    let rest = `${x.q}\n${x.a}`;
    const out = [];
    for (const [k, v] of [...this.glossaries.get(lang)].sort((a, b) => b[0].length - a[0].length)) {
      if (/^The\s/.test(k)) continue;
      const e = escapeRe(k), word = '(?![\\p{L}\\p{N}])', start = '(?<![\\p{L}\\p{N}])';
      const re = new RegExp(/\s/.test(k) ? `${start}${e}${word}` : `\\*\\*${e}\\*\\*|(?<=, )${e}${word}|${start}${e}(?=,)`, 'gu');
      if (!rest.match(re)) continue;
      out.push([k, v]);
      rest = rest.replace(re, '\u0000');
      if (out.length === GLOSSARY_MAX) break;
    }
    return out;
  }

  /** one model call: the question and its answer in this language, kept until either (or the glossary) changes */
  async translate(x, lang, terms, hash) {
    const glossary = terms.length ? `<glossary>\n${terms.map(([k, v]) => `${k} = ${v}`).join('\n')}\n</glossary>\n` : '';
    const res = await this.anthropic.messages.create({
      model: MODEL, max_tokens: 4000, system: SYSTEM,
      messages: [{ role: 'user', content: `Into: ${nameOf(lang)} (${lang})\n${glossary}${JSON.stringify({ q: x.q, a: x.a })}` }],
      output_config: { effort: 'low', format: { type: 'json_schema', schema: OUT } },
    });
    if (res.stop_reason !== 'end_turn') throw new Error(`the model stopped: ${res.stop_reason}`);
    const r = JSON.parse(res.content.filter(b => b.type === 'text').map(b => b.text).join(''));
    if (!r.q?.trim() || !r.a?.trim()) throw new Error('an empty translation');
    if (r.q.length + r.a.length + 10 > MAX_TEXT) throw new Error('the translation is longer than a message takes');
    (this.tr[lang] ??= {})[x.id] = { hash, q: r.q.trim(), a: r.a.trim() };
    this.saveTr();
    console.log(`[faq] ${x.id} into ${lang}: ${res.usage.input_tokens} tokens in, ${res.usage.output_tokens} out`);
    return this.tr[lang][x.id];
  }
}
