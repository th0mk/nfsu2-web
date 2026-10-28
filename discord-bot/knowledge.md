# Maintainer's notes

The player's guide (below these notes) is the reference for what the page and the game do. These notes add what the
guide does not say: how the project runs, what a useful report needs, the answers that keep coming up, and what is
known or decided. The code is public: https://github.com/th0mk/nfsu2-web.

## The project

- Players open https://nfsu2.xyz in Chrome or Firefox (Safari on iOS 26 / iPadOS 26 and macOS works too, less tested)
  and press Play. The server has the game files: players need no copy of their own and install nothing. Saves stay in
  the browser, or also on the server with cloud saves (a keyphrase only the player has).
- The page updates itself: before a game it loads the new version by itself; during a game it shows "A new version of
  the game is on the server", and Reload or Play loads it. A tab left open runs the old version until then. When a fix
  is live, the answer is when it was fixed, since when it is live, and "reload the page (Ctrl+F5 on a PC)".
- Multiplayer: free roam with everyone on the server (Online, Free roam with others), the game's Online mode (a name,
  no EA account) and races from free roam or the Online menus, up to 6 players. Crews, rooms, ghost mode (F9, solid
  cars), quick chat (1-4, then 1-4), horn (N).
- The game runs in 15 languages; the page too. Touch controls on phones and tablets; pads, wheels, 8BitDo and PS2
  adapters through the browser.
- Self-hosting is for people running their own server: the README (their own copy of the game, patched to v1.2 North
  America, with that exact SPEED2.EXE) and docs/web/VPS.md. "CANNOT OPEN" lines for game files in their log mean the
  game folder they gave the server is incomplete.

## Getting the log

The log is the most useful thing in a bug report. Say exactly how:

- During the game: F9, Download log. On a controller: Home, or both sticks pressed in. On a touch screen: the ☰ button.
- A freeze: wait about 5 seconds, then F9, Download log. The page asks the frozen game what each part is doing.
- After the game ended, crashed or was quit: "Download log" on the start page.
- iPhone or iPad, the game suddenly gone and the page loaded again: the start page says so, and Download log starts
  with that last game's log. Download it before playing again: only the last game's copy is kept.
- After a "The game was stopped" (GPU-LOST) or a new version: download the log before pressing Play or Reload, the
  reload clears it.
- Attach the .log file to the post (drag it into Discord, or the + button). Not a photo of the screen, not pasted text.
- What it contains, if they ask: browser and system, graphics chip, the time zone's UTC offset. Names (theirs, other
  players', crews, rooms) and addresses are replaced by stand-ins such as `you` and `player-…` since the change "Download
  log: no names or addresses" (once it is live; older pages' logs still have them). No passwords, no keyphrase.

## What a report needs, by problem

- Crash, freeze, black screen, stuck loading, car or world glitch: the log, what they were doing (career, free roam,
  race type, menu), and whether it happens again.
- A crash at one place in a career, or a profile that will not load: also the saves (Saves, Download backup: a zip),
  attached to the thread. The developer asks for that to reproduce it.
- The page shows a problem: the code it shows (`GPU-ADAPTER-WIN`, `PAGE-NOT-ISOLATED`, ...) or a screenshot of it. Most
  codes are in the guide's section 8 with what to do.
- The browser cannot use the graphics and the guide does not settle why: Chrome's report from `chrome://gpu` (the
  button at the top saves it as a file), attached.
- Graphics or speed: browser and version, device or graphics chip, the F8 frame-rate counter's numbers, where it
  happens (free roam with many cars, races, menus). The log has all but the last.
- Controller or wheel: the model, browser, system, wired or Bluetooth, and which button or axis does what wrong.
  Controls, Controller in the page menu shows what the page sees.
- Phone or tablet: the model, Android or iOS version, browser, Home Screen app or tab.
- Online or free-roam races: the log after the race (F9, Download log), who created the race, how many players, what
  went wrong (cars jumping, results missing, endless loading, disconnect).
- Saves and cloud saves: what happened, which browsers, whether cloud saves are on. Never ask for the keyphrase.
- Sound: browser and system, the output latency setting (Settings, Game, more), what is wrong (none, late, crackling).

## Answers that keep coming up

Checked against the guide or the code, and confirmed by players in threads:

- "It worked yesterday, today it won't start" or a crash on an older version (the log's `game <rev>` is not the live
  one): reload the page, Ctrl+F5 on a PC; when a fix for it is live, say when it was fixed and since when it is live.
- `GPU-ACCEL-OFF`, or Chrome drawing with "Microsoft Basic Render Driver" or SwiftShader: turn on "Use graphics
  acceleration when available" in `chrome://settings/system` and restart Chrome. Moderators answer this often; it works.
- `PAGE-NOT-ISOLATED` ("not cross-origin isolated") on nfsu2.xyz itself: something in the browser changes the page,
  most often an extension. A guest or incognito window works for the players who tried it; then turn the
  extensions off one by one.
- Saves gone after closing the browser: the browser deletes site data when it closes (Chrome: `chrome://settings/content/siteData`,
  allow nfsu2.xyz to save data). Lost saves do not come back; Saves, Download backup, and cloud saves keep a copy.
- Loading a backup or a PC profile: Saves, Restore a backup (the guide's section 5).
- Saving on a phone: the game's own save, Garage, Save; it saves by itself after races and purchases from then on.
- The site or the server cannot be reached while others play: their network blocks it (some providers block
  Cloudflare or the domain; firewalls and antivirus can too). Mobile data or another network shows it. The server is
  behind Cloudflare: wherever Cloudflare works, the game does. Other players posting in the last hour shows the
  server is up.
- Stuck behind a barrier or off the map: R respawns.
- Career, no races left on the map (and nothing on the radar): the races left are the original game's hidden races.
  They never show on the map or the radar, only on the street: their marker appears when you drive near. Drive
  around the city, the newer areas too. Someone who still finds none can attach the saves (Saves, Download backup):
  escalate it, the developer can tell where they are. (The developer answered this on 2026-10-26.)
- Firefox: when a problem shows only in Firefox (its log has Firefox errors, or Chrome works), Chrome is the browser
  most tested.
- A phone that stutters or shakes at 60 fps but runs smoothly at 30: its GPU no longer finishes a frame in 16.7 ms,
  often only after a few minutes, as the phone warms up (the log: `[gpu] … waited over 20 ms for the GPU` lines,
  `GPU done p50` above 16 ms). Settings, Display, Frame rate 30 is the steady choice for such a phone. To keep 60: the
  lighter graphics. A phone starts with them, but not one where a graphics option was changed before (the log says
  `phone graphics: the graphics options were changed in this browser, they stay`); then the game's Options, Display:
  car and road reflections, motion blur, depth of field, overbright and bleach bypass off, and the resolution on the
  row with a size such as 1916x884 (the screen's own at the page's resolution scale), not 1080p or 1440p, which draw
  more pixels than a phone's default 75%.

## Known open problems (as of 2026-10-28)

The developer updates this list. Check the commits after that date before calling one open: a fix may have come. The
context lists every problem the developer has a report on, with its first thread: point posters there, and set
`duplicate_of` to it, rather than searching for "the main thread".

- iPhone and iPad: the browser closes the game for memory, and the start page comes back (worst on 4 GB iPhones, 12
  to 14, but newer ones too); some logs end in `GPU-LOST` instead. Several improvements are live; it still happens.
  No setting fixes it. Ask for the log from the start page right after it happened, and point to the report on it;
  escalate only a log on the live version that adds something.
- Huawei phones and tablets with Maleoon graphics: the unsafe-webgpu flag lets the game start, then the graphics
  stop (`GPU-LOST`) within half a minute to a few minutes; frame rate 30 does not help. No fix.
- Android phones with 2 to 4 GB of memory: the game does not start, the browser cannot reserve its memory ("could
  not allocate memory" in the log) on every Play. No fix.
- Phones with PowerVR BXM graphics (MediaTek Dimensity 7020/7025): the graphics stop within seconds in landscape
  (`VK_ERROR_OUT_OF_DEVICE_MEMORY` in the log); portrait works for some.
- Black picture with sound on Intel graphics of the 9th generation (HD 510 and similar), in Chrome and Firefox: the
  log shows the game drawing with no WebGPU errors. No fix.
- Free roam: the world's loading screen, or roads missing, while driving and while many players' cars load, most
  in Firefox and with the game data on a slow disk. Car loads got faster on 27 October; a log from after that helps.
- Back to free roam after a race with other players: endless loading, or a crash (`INVALID FREE` and a `TRAP` after
  the lobby screen in the log).
- Sound on a PC that goes silent after the game was paused or its window inactive (`the AudioWorklet stopped
  consuming` in the log, on the live version). On phones, sound that chopped up got a fix on 26 October: a report
  of that after it is new.
- Radio with a music folder: on some devices the songs play slowed down. Ask for the log after a few songs, and the
  files' type (mp3, flac, ...).

## Decided

The developer said these in public; repeat them briefly and kindly when a post asks for exactly one, otherwise none:

- No other games will be revived by this project.
- No text or voice chat: it needs moderation. Quick chat (1-4, then 1-4) is there instead.
- No cloud streaming of the game: too complex and too expensive.
- No music hosted on the server; music from the player's own device may come (it is being worked on).
- No account system; cloud saves (a keyphrase) sync saves between browsers.
- Free roam stays one world, not split into sessions.
- The game's own mechanics stay, wall riding in races included (next section).

## What changes and what doesn't

The game plays as the original did. Additions and improvements around it are welcome: multiplayer features, quality of
life, controls, the page, languages, optional graphics settings (off by default: the original look stays the default),
fixes of the original game's crashes and glitches. Changing its core gameplay mechanics is not done: handling and physics, car
performance and tuning, AI and traffic, the career's progression, races and rewards, the economy.

- A suggestion of an addition or improvement: none (the developer reads them), unless it exists already: then say
  where (Power Units in Options, Gameplay; drift mode with X; wheels; quick chat; ghost mode in F9; Upscaled HUD in
  Settings, Display; photo mode with F12 or the pause menu's Photo Mode, from the release after 8d42385 on; a garage
  of 10 cars, 2 slots more each stage, and Trade Car on the crib's car list, from the release after 06ff511 on; the
  guide lists them all).
- A suggestion that would change a core mechanic: you may answer, briefly and kindly, that the project keeps the
  original game's mechanics as they are, so it won't change that. Only when it clearly is one; otherwise none.
- A "bug" that is how the original game behaves (drift races have no music; a part's info screen; hidden races): say so
  when you are sure, or when the guide or the code shows it; otherwise none.

## Limits players hit (true as of these notes; check the guide and the code)

- No WebGPU, no game: Firefox for Android (play in Chrome), Samsung TV browsers, Windows PCs without Direct3D 12
  (Intel HD 4000 and older). Chrome on Linux needs the Vulkan driver. Players report that the Xbox's browser does not
  run it either.
- Samsung Xclipse graphics (some Galaxy phones; `GPU-ADAPTER-ANDROID`, chrome://gpu says "Limited support / testing
  currently available on Android"): Chrome blocklists them, and chrome://flags/#enable-unsafe-webgpu, then restarting
  Chrome, lifts that. A player's Xclipse 540 (Android 16; the Exynos 1580 in the Galaxy A56) runs the game with it
  (2026-10-26); other Xclipse chips are not tried yet. On the phone: type the address into Chrome's address bar, or
  open chrome://flags and search for "Unsafe WebGPU". Don't tell players to set it back to Default afterwards: it
  doesn't matter. The flag is worth a try for any `GPU-ADAPTER-ANDROID` (the page says so from the release after
  c0b379c on), not only Xclipse.
- iPhone: a web page cannot go full screen. Adding the page to the Home Screen (Share menu) opens it without the
  browser's bars. That copy has its own saves: cloud saves bring them over. Only controllers iOS knows (Xbox,
  PlayStation, Switch, MFi) reach the page.
- Wheels: no force feedback in any browser; the clutch does nothing (the game has none).
- Stuttering cars in races with other players: all updates go through the player who created the race, so the one with
  the steadiest connection should create it, and keep the game's tab in front.
- One game per browser: a second tab shows "Open elsewhere".

## Don't

- Don't tell players to edit settings.ini variables (`NFSU2_...`) or use `?env=` links: those work only on a
  development server. Don't send them to `chrome://flags` unless the guide says so for their case.
- Don't suggest mods, trainers, cheats, save downloads, other builds or other sites.
- Don't answer questions about moderation, bans, kicks, names, donations, Discord channels, server load or the
  server's setup: staff answer those (none).
- Don't guess at release plans or say what the developer will do, beyond the decisions above.
