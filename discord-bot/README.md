# Forum bot

A Discord bot for the server's forums. It looks at each new post once its author has stopped adding to it (2 minutes),
and it says something only when it is worth it:

- **in the thread**, a fix it verified in the player's guide, the code or the git history (an error code, a missed
  setting, a browser's limit, a bug fixed in the live version, with when it was fixed and since when it is live); or
  a request for what the report lacks (the log, the browser and device, the controller's model), with how to get it.
  In the poster's language, dates as Discord timestamps (each reader sees their own time zone), web addresses in
  inline code; a reply too long for one message goes out as several, split between its parts;
- **in the staff channel**, a report the developer should act on, with everything needed to start: what happens,
  device, browser and version, the key log lines, the likely cause, the thread's link and its logs, pictures and saves.
  The report is a post (in a text channel: a message with a thread on it) that the bot rewrites at every change:
  the problem; where it stands, 🔴 **Open**, ✅ **Fixed** (a release's fix is live and the bot told a thread; whether
  a poster confirmed it), ✅ **Solved** (the poster says it works) or 🟠 **Reported again** (a new thread or log
  after that), the post's name starting with the same mark; its forum and category (`bug-reports · audio`); every
  thread about it, in one line; its files with links to download them; and one line with a Discord timestamp for
  each change. The post's thread holds the rest: first the part for Claude, a code block of plain text (the threads,
  the notes, the files' names) that Copy Text or a selection takes without Discord's markup, with the first thread's
  files; the bot edits it as updates come, so one copy has them all. Then each update with its files: later threads
  about the same problem (with what they add), new logs or pictures in any of them, a fix that went live, the poster
  saying it works. Only the thread's followers are notified. A report from before this layout gets it at its next
  change: its text and files move into its thread. Spike alerts and daily notes are posts of their own.

- **when a fix goes live**, in a thread whose problem one of the release's commits fixes: that a fix may have solved
  it, what the fix changes (one plain sentence, no internals), to reload the page, and to post a new log if it still
  happens. Only when a full look at the thread is sure; at most once a thread, at most 3 threads a release (short,
  low-effort looks), threads of the last 14 days, also threads staff wrote in (one the bot never looked at is matched
  by its title and first post). A report about the thread turns ✅ Fixed.

- **when a suggestion is carried out**, in a suggestion thread (a forum named `suggest…`, or a post the bot took for a
  suggestion) that one of the release's commits does what it asks for: that it was added, since when it is live, where
  to find it, and to reload the page; then it tags the thread Complete. Only when a full look is sure (the whole wish
  or its main part, not something related); at most once a thread, at most 5 threads a release. A release that takes
  a clear step toward a suggestion without doing it (more quick-chat phrases for a wish for text chat) gets one or
  two sentences on what it adds, once a thread, and the thread stays open. Old suggestions count
  too: at each release the bot reads the suggestion forums' threads, archived ones included (the newest 500 of each),
  and keeps them a year. Suggestions tagged Complete or Denied, or locked, are left out.
  At start and every 3 hours it also counts each suggestion's votes, for the developer: the reactions on its first
  post and Discord's count of the thread's messages and people (`votes`, `messages`, `members` in `state/threads.json`;
  the model doesn't see them).

- **in the general channels** (text channels named `general…`), sparingly: someone who reports a problem of their own
  there, and whom nobody helps within 4 minutes, gets one short reply in their language. When the problem is
  unmistakably one the page names by a code (`PAGE-NOT-ISOLATED`, `GPU-ADAPTER-ANDROID`, ... in their words or their
  screenshot), or one the guide answers in a line (a name that got four digits, a TV's or a console's browser;
  `FIXES` in `general.mjs`), the reply is its one-line fix from the guide; otherwise it points to the forum
  thread about it or to the bug-reports forum, with what to post there (the log, a screenshot of the message, or how
  the saves went). Nothing for questions, chat, opinions or answers to others, nothing when staff or another member
  already pointed them to the forum or a member's answer fits (a reply to them starts the wait over); at most once a
  person a week and 6 an hour a channel; nothing for someone talking it over with members (a second member helping
  them, or them answering a member: for 30 minutes), nothing 20 minutes after their first message, and no pointer
  to their own thread. A local word filter picks the candidates (about 10% of the messages), and
  one Haiku call sorts them in the chat around them (what they reply to, who answered, their pictures): about 70 calls
  and 3 cents a day. `general-eval.mjs` replays an export of the channels through it (below). Spikes of complaints
  (connection, crashes, controllers, sound, graphics: 8 people or more in 15 minutes, far above the 6 hours before)
  go to the staff channel at once, without a model, and a daily note at 06:00 UTC sums up what the general channels
  kept bringing up (threads as links, in two messages when it runs long).

- **The developer's answer, once for every thread**: mention the bot in a report's thread in the staff channel (or
  in a reply to the report's message) with the answer after the mention, such as "@NFS Helper these are hidden races:
  they show on the street when you drive near". The bot posts it in every open forum thread of that report, each in
  its poster's language (one Haiku call), starting "From the developer:", with the message's files (4 at most); the
  report gets a line with where it went, and the bot's own looks stay out of those threads as if staff had answered.
  A mention outside a report's thread, or without an answer, gets a one-line hint. In a dry run the posts go to the
  staff channel.

- **Translate**, in any message's Apps menu (right-click, or a long press on a phone): the message and its
  translation in one box, a divider between them and nothing else, seen only by the one who asked. A message not in
  English is translated into English; one in English into the thread's language (in a plain channel, the language the
  others there write in), or into the reader's own Discord language when the thread is English too. One short Haiku
  call, and every translation is kept 30 days in `state/translations.json`: a message costs one call however many
  people translate it, and a thread's language is kept too, so later translations in it send no other messages
  along. At most 30 new translations a person an hour.

- **Forum tags**: each look adds the forum's tags that say what the thread is about, at most two (the problem, and the
  device when it is a phone or tablet), picked from the forum's tags but the five below, the moderated ones and
  Denied. Five it keeps itself, by name, in a forum that has them: **Needs info** while its last reply asked the poster
  for something, **Dev aware** once the developer has the report, **Fix live** once it told the thread that a fix went
  live, **Complete** once it told a suggestion that it was added, and **Solved** when the poster says the problem is
  gone (a message of theirs that passes a word filter, then one short Haiku call over the end of the thread; also in
  threads staff answered; a report about the thread then says so, also in a dry run). It takes off only its own Needs info and Fix live: the poster's and staff's tags stay. It needs Manage Threads in the forums (Set up, step
  3); nothing in a dry run (the log says what it would set).

Otherwise it says nothing. It reads every channel it can see, but posts only in forum threads, those replies in the
general channels, and the staff channel. It
stays out of a thread once staff wrote in it (the server owner, `BOT_STAFF`, anyone who can manage threads), and replies
at most twice in a thread on its own. Someone answering it gets a look past that, up to `BOT_MAX_TALK` replies (4):
after it asked for something, the poster's next post, which always gets a reply (a fix, or what their log shows and
that the developer has it), so nobody who did what it asked hears nothing; and the poster writing right after its
message, with Discord's reply or without ("that didn't work", "where is that setting?"). A bare thanks or okay gets no
look, and the poster still writing while a look waits makes it wait for them. A look
that repeats a tool call word for word gets told so, and the second time it has to answer; so does a look whose last
three calls brought nothing (an error, no matches, a line or two of a file). Staff can mention
the bot in a thread to have it look anyway. A bump, the poster writing again after the thread was quiet for 12 hours
("any ideas?"), gets a look and a reply too (a fix, a question, or where it stands), also in a thread staff wrote in
before, unless staff answer it first; bumps while the bot was away get theirs at start. A log (a `.log` or `.txt`
file) posted in a thread by anyone but staff always gets a look, also past the two replies and where staff wrote. Anyone
who replies to the bot's message (with the ping or without) or mentions it gets a look too, and an answer when there is
something to say, until the bot has replied `BOT_MAX_TALK` times (4) in the thread. The same log file posted again
(same name and size) gets no look, and a reply that mostly says again what the bot said in the thread (half its word
pairs) is dropped. When the poster disputes the bot, is upset with it, or the talk goes in circles, the look steps
back (`handoff` in its verdict): at most one line that the developer will read the thread, a "Needs a person"
report or update in the staff channel, and no more looks in that thread unless staff mention the bot (a fix notice
still goes out; new logs still reach the report).

Every model call goes on a meter (`spend.mjs`, `state/spend.json`): looks, sorting, tags, translations, release checks,
against `BOT_BUDGET` dollars a month (200, UTC months). Only the looks wait for money: each day they may spend what is
left of the month over the days left in it, so a quiet day leaves more for the rest and a busy week can't spend the
month. A look past today's share waits for the next day and runs then; staff's mentions still get theirs at once. The
bot's first log line says what the month and the day spent so far.

## How it works

- `bot.mjs`: Discord (discord.js). It decides when to look, builds the thread's text, saves attachments, inlines
  pictures (PNG, JPEG, GIF, WebP up to 10 MB, 4 a look) and posts. Each picture goes to the model at most 1,568
  visual tokens big (`picture()` in `investigate.mjs`, with sharp), the size Claude read pictures at before Opus 4.7:
  a phone screenshot costs 1,456 tokens instead of 3,354, a 4K one 1,560 instead of 4,784.
- `investigate.mjs`: one look. It sends Claude the rules (`system.md`), the maintainer's notes (`knowledge.md`),
  `docs/web/PLAYING.md`, the live version, recent commits, recent threads and the problems the developer has a report
  on (each report's first thread, where it stands; what it points posters to and joins duplicates to), and the
  thread with the files it can read named at its top. The rules, the notes, the guide and the code's history (the
  same for every look until the next commit or release) are kept an hour in the prompt cache, so a look reads them
  for a tenth of the price instead of sending them again. Claude reads what it needs through the
  tools, then returns a verdict as JSON: none, ask or answer, and whether to tell the developer. `vet()` drops weak
  answers and replies that paste code.
- `tools.mjs`: what Claude can do. It can search and read the code, read the thread's attachments, look at the git
  history (`LIVE` is the live server's commit), search the forums' threads (to point a duplicate to the earlier one)
  and the archive of recent messages (the staff channel's marked private: the model may use them, never repeat them).
  Everything is read only: no shell, no network, no writing, and nothing outside
  the bot's own copy of the code and the attachments.
- `repo.mjs`: the bot's own clone of the code in `state/repo`, fetched at most every 5 minutes. The rules, the notes
  and the guide are read from that clone, so a pushed change to them applies within minutes, without a new image or a
  restart. The live version comes from the module file, else `https://nfsu2.xyz/game.json`; next to the module file,
  `release.sh`'s `release.json` names the running image's commit, which is the live version while it names that module
  (a release of only the page or the server keeps the module's `nfsu2.json`). Each new live version goes into
  `state/releases.json` with its time (the file's, which `release.sh` writes as the release goes out), and every
  commit the model reads shows when it went live.
- `archive.mjs`: every message of every channel the bot sees, kept 30 days in `state/archive/` (the model searches the
  last 7). At start the bot reads the forums' threads with posts in the last 48 hours, nothing older: it lists them
  (titles and first posts) in `state/threads.json`, and looks at the ones opened in that time that it never looked
  at, so a restart misses no post.
- `general.mjs`: the general channels: the replies that point to the forums or give a page code's fix, the spike
  alerts and the daily note.
- `translate.mjs`: the Translate command. The bot registers it at start; the `bot` scope of the invite covers it.
- `tags.mjs`: the forum tags. A look's verdict names the tags it picked from "Tags you may add" in the thread's text.
- `eval.mjs`: the bot against past threads (below).

`docker compose logs -f` shows each step as it happens: when a post is seen and when its look is due, what the look
reads (each search and file, and how much it found), the verdict, what was posted, and what went to the staff. Each
look also adds one line to `state/verdicts.jsonl`: the verdict with the developer notes, what was posted (or why not),
the tools used, the tokens and the cost.

## Set up

1. **Discord application.** In https://discord.com/developers/applications, create an application. Under Bot:
   - reset the token and copy it;
   - turn on **Message Content Intent** (no other intent is needed);
   - turn off Public Bot, so only you can invite it.

   Invite it with
   `https://discord.com/oauth2/authorize?client_id=<application id>&scope=bot&permissions=274877973504`.
   That gives it View Channels, Read Message History and Send Messages in Threads, and nothing else. Without Send
   Messages it cannot write in ordinary channels or open posts. If a forum hides itself from @everyone, give the bot
   View Channel there too. For the replies in the general channels, allow it Send Messages in those channels'
   permissions; without that it only counts them for the staff.
2. **Staff channel.** Make a private channel for staff. In its permissions, add the bot with View Channel, Send
   Messages, Attach Files, Read Message History, Create Public Threads and Send Messages in Threads. Turn on Developer Mode (User Settings, Advanced), right-click the
   channel, Copy Channel ID.
3. **Forum tags.** In each forum's permissions, add the bot with Manage Threads: only the poster and those who can
   manage threads can set a post's tags. Without it the bot says so once in the log and goes on without tags. It
   picks from the tags each forum has, so a new tag needs nothing else; name its own five exactly Needs info, Dev
   aware, Fix live, Complete and Solved (any letter case). Tags players should not set themselves (Needs info, Dev
   aware, Fix live, Complete, Denied): "Only allow moderators to apply this tag" in the forum's tag settings. The bot
   never picks a moderated tag, its own five aside.
4. **Claude.** Create an API key at https://console.anthropic.com, and set a monthly spend limit there.
5. **`.env`**:

   ```sh
   DISCORD_TOKEN=...
   ANTHROPIC_API_KEY=...
   BOT_STAFF_CHANNEL=...         # the staff channel's id
   BOT_DRY_RUN=1                 # first: replies go to the staff channel, not the forums
   # BOT_FORUMS=123,456          # only these forum channels (default: every forum)
   # BOT_STAFF=789               # more staff: user or role ids
   ```

## In Docker

`.github/workflows/image-bot.yml` builds `ghcr.io/th0mk/nfsu2-web-bot` on every push that changes this folder. On the
server, in a folder of its own (`/srv/nfsu2-bot`, say), put `compose.yml` from here and the `.env` above, then:

```sh
docker compose pull && docker compose up -d
docker compose logs -f
```

The bot clones the public repository into its `state` volume, and reads the live version from the game server's module
folder (`/srv/nfsu2/module`, read only): Cloudflare answers the server's own requests for game.json with a check page. If the pull is denied, the image is private on GitHub:
make it public in the package's settings, or log in once with `docker login ghcr.io -u th0mk` and a token with
`read:packages`.

- New code: `docker compose pull && docker compose up -d`. A change to `system.md`, `knowledge.md` or the guide needs
  nothing: the bot reads them from its clone.
- After a change to `.env`: `docker compose up -d`.
- One post without Discord: `docker compose run --rm bot node try.mjs --title "Game crashes" "it crashes in free roam"`.
  With a log: add `-v "$PWD/x.log:/tmp/x.log:ro"` after `--rm` and `/tmp/x.log` at the end.

## Without Docker

Node 22.9 or newer and git, in this folder:

```sh
npm ci && npm start
node --env-file=.env try.mjs --title "Game crashes" "it crashes in free roam on my iphone" nfsu2-2026-10-25_151223.log
```

It clones the checkout this folder is in (its `main` branch, so committed changes only), unless `BOT_REPO` says
otherwise.

## Trying it on past threads

`eval.mjs` runs past threads through the bot, each as a new post, and writes a report next to the cases with the bot's
reply, its staff note, and what staff or members actually answered:

```sh
node --env-file=.env eval.mjs cases.json --staff --limit 20     # 20 threads that staff answered
node --env-file=.env eval.mjs cases.json --only bug-reports/015,suggestions/008
```

`cases.json` is made from a Discord export and stays out of this repository: it holds players' posts and logs. It
stops at `--budget` dollars (default 5); a look costs a few cents.

`general-eval.mjs` does the same for the general channels: an export of them (a JSON list of messages, `{ id, ch, t,
a, c, re: { a, c }, att, bot }`) replayed in order on its own clock, with the real sorting, and a report next to it of
every reply the bot would post and how it sorted every author. A day costs about 3 cents; the export has no pictures,
so the sorting reads none.

```sh
node --env-file=.env general-eval.mjs nfsu2-general-last24h.json --staff name,name
node --env-file=.env general-eval.mjs export.json --from 2026-10-26T12:00 --to 2026-10-26T18:00
```

## Rolling it out

Run it with `BOT_DRY_RUN=1` for a day. Every reply it would have posted goes to the staff channel instead, quoted, and
`state/verdicts.jsonl` has every look. When the replies look right, remove the line and run `docker compose up -d`.

## Settings

| Variable | Default | |
|---|---|---|
| `BOT_STAFF_CHANNEL` | | the staff channel's id; without it, reports for the developer are only in `verdicts.jsonl` |
| `BOT_DRY_RUN` | off | `1`: post nothing in the forums or the general channels, and set no tags; forum replies go to the staff channel, general-channel ones only to the log and the daily note |
| `BOT_FORUMS` | every forum | forum channel ids to answer in |
| `BOT_STAFF` | | user or role ids of staff, besides the owner and anyone who can manage threads |
| `BOT_MODEL` | `claude-sonnet-5-5` | |
| `BOT_EFFORT` | `medium` | `low` … `max`: how much the model thinks and reads before it answers |
| `BOT_MIN_CONFIDENCE` | `medium` | answers below this confidence are not posted (questions are) |
| `BOT_WAIT_S` | `120` | quiet time after the poster's last message before the look (at most 10 minutes after the first) |
| `BOT_MAX_REPLIES` | `2` | replies per thread on the bot's own (logs, bumps and staff's mentions get a look past it) |
| `BOT_MAX_TALK` | `4` | replies per thread in all, answering people who reply to the bot or mention it |
| `BOT_BUDGET` | `200` | dollars a month for every model call; looks past the day's share wait for the next day |
| `BOT_ARCHIVE` | on | `0`: don't keep messages (the model then can't search other channels) |
| `BOT_STATE` | `state` (`/state` in Docker) | where the clone, the archive, the thread list, the verdicts and the translations go |
| `BOT_REPO`, `BOT_BRANCH` | this checkout, `main` | where the code comes from (in `compose.yml`: GitHub over SSH) |
| `BOT_LIVE_FILE` | `/live/nfsu2.json` | the live module's `nfsu2.json`, for its version (`rev`), and `release.json` beside it (the image's commit); `compose.yml` mounts `/srv/nfsu2/module` there |
| `BOT_LIVE_URL` | `https://nfsu2.xyz/game.json` | the live version when there is no such file (Cloudflare may refuse it to a server) |

Server-side fallback (`fallbacks: "default"`) is on: when Claude declines a request, the API runs it on another model
instead of returning nothing.
