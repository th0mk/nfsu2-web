# Maintainer's notes

The player's guide (below these notes) is the reference for what the page and the game do. These notes add what the
guide does not say: how the project runs, what a useful report needs, the answers that keep coming up, and what is
known or decided. The code is public: https://github.com/th0mk/nfsu2-web.

## The project

- Players open https://nfsu2.xyz in Chrome, Edge (or another Chromium browser: Brave, Opera) or Firefox (Safari 26 on a
  Mac, iPhone or iPad works too). Chromium browsers run the game as Chrome does: the `BROWSER-CHROMIUM` warning ("Only
  Chrome and Firefox are tested") is harmless, and the release after 371f501 no longer shows it, nor one for Safari.
  and press Play. The server has the game files: players need no copy of their own and install nothing. Saves stay in
  the browser, or also on the server with cloud saves (a passphrase only the player has; the page called it a keyphrase before 2026-10-29, so players may say either).
- The page updates itself: before a game it loads the new version by itself; during a game it shows "A new version of
  the game is on the server", and Reload or Play loads it. A tab left open runs the old version until then. When a fix
  is live, the answer is when it was fixed, since when it is live, and "reload the page (Ctrl+F5 on a PC)".
- Multiplayer: free roam with everyone on the server (Online, Free roam with others), the game's Online mode (a name,
  no EA account) and races from free roam or the Online menus, up to 6 players. Crews, rooms, ghost mode (off by
  default: cars collide; F9 switches it), quick chat (1-4, then 1-4; a controller's right stick pressed in, then
  flicked twice; its special phrase Countdown counts 5 to GO! for the players lined up near you), horn (N).
- The game runs in 16 languages (English and 15 translations); the page too. Touch controls on phones and tablets;
  pads, wheels, 8BitDo and PS2 adapters through the browser.
- Self-hosting is for people running their own server: the README (their own copy of the game, patched to v1.2 North
  America, with that exact SPEED2.EXE) and web/deploy/. "CANNOT OPEN" lines for game files in their log mean the
  game folder they gave the server is incomplete.

## Getting the log

The log is the most useful thing in a bug report. Say exactly how:

- During the game: F9, Download log. On a controller: Home, or both sticks pressed in. On a touch screen: the ☰ button.
- A freeze: wait about 5 seconds, then F9, Download log. The page asks the frozen game what each part is doing.
- After the game ended, crashed or was quit: "Download log" on the start page.
- iPhone or iPad, the game suddenly gone and the page loaded again: the start page says so, and Download log starts
  with that last game's log. Download it before playing again: only the last game's copy is kept.
- After a "The game was stopped" (GPU-LOST) or a new version: download the log before pressing Play or Reload. After
  a reload, Download log still starts with a shorter copy of that game's log (only the last game's, and not after a
  game they quit normally).
- Attach the .log file to the post (drag it into Discord, or the + button). Not a photo of the screen, not pasted text.
- What it contains, if they ask: browser and system, graphics chip, the time zone's UTC offset. Names (theirs, other
  players', crews, rooms) and addresses are replaced by stand-ins such as `you` and `player-…` since 26 October (logs
  from older pages still have them). No passwords, no passphrase.

## What a report needs, by problem

- Crash, freeze, black screen, stuck loading, car or world glitch: the log, what they were doing (career, free roam,
  race type, menu), and whether it happens again.
- A crash at one place in a career, or a profile that will not load: also the saves (Saves, Download backup: a zip),
  attached to the thread. The developer asks for that to reproduce it.
- The page shows a problem: the code it shows (`GPU-ADAPTER-WIN`, `PAGE-NOT-ISOLATED`, ...) or a screenshot of it. The
  guide's section 8 has every code with what to do.
- The browser cannot use the graphics and the guide does not settle why: Chrome's report from `chrome://gpu` (the
  button at the top saves it as a file), attached.
- Graphics or speed: browser and version, device or graphics chip, the F8 frame-rate counter's numbers, where it
  happens (free roam with many cars, races, menus). The log has all but the last.
- Controller or wheel: the model, browser, system, wired or Bluetooth, and which button or axis does what wrong.
  Controls, Controller in the page menu shows what the page sees.
- Phone or tablet: the model, Android or iOS version, browser, Home Screen app or tab.
- Online or free-roam races: the log after the race (F9, Download log), who created the race, how many players, what
  went wrong (cars jumping, results missing, endless loading, disconnect).
- Saves and cloud saves: what happened, which browsers, whether cloud saves are on. Never ask for the passphrase.
- Sound: browser and system, the output latency setting (Settings, Game, More game settings), what is wrong (none, late, crackling).

## Answers that keep coming up

Checked against the guide or the code, and confirmed by players in threads:

- "It worked yesterday, today it won't start" or a crash on an older version (the log's `game <rev>` is not the live
  one): reload the page, Ctrl+F5 on a PC; when a fix for it is live, say when it was fixed and since when it is live.
- `GPU-ACCEL-OFF`, or Chrome drawing with "Microsoft Basic Render Driver" or SwiftShader: turn on "Use graphics
  acceleration when available" in `chrome://settings/system` and restart Chrome. Moderators answer this often; it works.
- `PAGE-NOT-ISOLATED` ("not cross-origin isolated") on nfsu2.xyz itself: something in the browser changes the page,
  most often an extension. A guest or incognito window works for the players who tried it (its saves are gone when it
  closes: it only shows the cause); then, in a normal window, turn the extensions off one by one.
- Saves gone after closing the browser: the browser deletes site data when it closes (Chrome: `chrome://settings/content/siteData`,
  allow nfsu2.xyz to save data). Lost saves do not come back; Saves, Download backup, and cloud saves keep a copy.
- Lost the cloud saves passphrase: since the release after 04a5c3f, Chrome and Edge offer to keep it with the browser's
  passwords (user name "Cloud saves"), which survive cleared site data. Look in the browser's password manager under
  nfsu2.xyz; a browser with no saves offers them back as the page opens, and the Cloud saves box fills it in for Load.
- Loading a backup or a PC profile: Saves, Restore (the guide's section 5).
- "Restoring stopped halfway: … Restore the backup again before playing" with no profiles left, on a page from before
  the release of 2 November, 22:47 UTC: that restore emptied the save folder first, then wrote the backup's files, so
  a write the browser refused (any error, `InvalidStateError` in Edge among them) left the folder empty. Not a fault
  of the newer restore: since 0181d97 it writes the backup beside the save folder and moves it in all or none, so a
  refused one leaves the saves as they were. Reload the page (Ctrl+F5 on a PC) and restore again. On a version with
  it, a restore that leaves the profiles empty is new: escalate it with the log and the backup.
- Progress not saved (often on a phone): a new profile starts on the game's Save = Prompt, whose "save?" question
  after a race opens on No, so tapping through skips the save. Pick Yes there once, or set the game's Options,
  Gameplay, Save (also in the pause menu's Options) to Autosave. The page sets each profile to Autosave by itself at
  the first Play after the profile was saved; from then on it saves after races, purchases and changes.
- The site or the server cannot be reached while others play: their network blocks it (some providers block
  Cloudflare or the domain; firewalls and antivirus can too). Mobile data or another network shows it. The server is
  behind Cloudflare: wherever Cloudflare works, the game does. Other players posting in the last hour shows the
  server is up. Known cases, each confirmed by players:
  - Spain during LaLiga football matches (weekends, afternoon to late evening): Spanish providers block Cloudflare's
    addresses by court order, so the page can't reach the server or stays at loading. It works again after the
    match; players got in with a VPN meanwhile. Ask whether they are in Spain before anything else.
  - NordVPN's Threat Protection, antivirus web filters and some school or work networks flag the new `.xyz` domain:
    the page goes Offline and Cloud saves disappear. Turn the filter off or allow `nfsu2.xyz`.
  - Kaspersky (Standard, Plus) flags `nfsu2.xyz` or its main.js as a malicious link or a trojan since 2 November: a
    false positive, which staff reported to Kaspersky. The start page stays at "Loading the game" in every browser.
    Adding `nfsu2.xyz` to Kaspersky's trusted addresses, or turning its web protection off, fixed it for several
    players.
  - "This site can't be reached" / the address can't be found (DNS) on a school, university or work network: setting
    the device's DNS to 1.1.1.1 or 9.9.9.9 fixed it.
- Stuck behind a barrier or off the map: R respawns.
- Career free roam with a black world and only the HUD (empty radar, often lower frame rate; menus and quick races
  fine), often after a hard hit from another player: the car was lost off the map and the profile kept that spot, so
  it came back at every Play. Fixed on 1 November (0f17d0a), live since the 2 November release: reload the page and
  Play, and the car starts at the airport car park. On an older page, R respawns.
- Sound late: Settings, Game, More game settings, Output latency "Lowest" (the default) is the shortest; Balanced
  and Robust add delay and are for crackling or dropouts. Never suggest Robust for late sound. Bluetooth headphones
  add their own delay.
- The EA Trax songs differ from the original game's (other versions, longer intros, "covers", swear words): they are
  the uncensored versions on purpose: unedited, some in longer album versions. Since 32540c78, Settings, Game, Songs,
  Original plays them as the game shipped them, and F7 (keyboard only) plays the next song.
- A laptop with a graphics card that plays badly or shows a black picture, while its log names the Intel or AMD
  integrated chip: Chrome runs on the wrong one. Windows Settings, System, Display, Graphics: add Chrome, set High
  performance, then quit Chrome completely and start it again. Players confirmed it.
- Touch Nitro or the thumb stick do nothing while Gas and steering work: they act as the controller's buttons, so
  Settings, Controller, More controller settings, Controller must be On or Hotplug, not Off. A poster confirmed it.
- Steam Deck or Steam Controller: buttons act as arrow keys, Enter and Esc and no pad shows in Controls, Controller:
  Steam Input is in its keyboard mode; add Chrome to Steam as a non-Steam game with a gamepad layout and start it
  through Steam (confirmed). The game crawls on a Steam Deck with sound playing: Chrome draws WebGPU on SwiftShader
  (`chrome://gpu`: Vulkan Disabled); a player fixed it by starting Chrome with
  `--enable-features=Vulkan --use-angle=vulkan` (2026-11-01).
- A pad that Windows sees but Controls, Controller does not: close Steam, DS4Windows and other pad software, press a
  button with the page in front, Controller On or Hotplug. If it still shows none, try Edge: on 3 November Opera GX
  and, on one PC, Chrome never handed a PS5 pad to the page, while Edge did (both players confirmed). The page's own
  controller settings (Controller On, Hotplug or Off, dead zones, trigger full press, rumble, a wheel's set-up) are in
  Controls, Controller (the start page, or F9 in the game).
- Changing a controller's buttons, asked every day ("it only shows the keyboard configuration"): the game's Options,
  Controls opens on Keyboard Configuration; Up to that title at the top, then Right, shows Joypad Configuration, the
  controller's own buttons (checked: tests/scripts/pad_controls.txt). The game lists a controller only once it has
  seen it: press a button on it before opening Controls (Controller On or Hotplug). Its buttons are kept with the
  profile (leaving Options saves them), and since 5755e96 (live 2 November) through the pad switching itself off and
  on; the keyboard's keys are kept for every profile.
- Quick Race's Free Run has no world map in the original game: M does nothing there. The world map is in a career's
  free roam.
- Turning the car in the garage or a shop: drag with the middle mouse button (the wheel) held, two fingers on a
  touch screen, or a controller's right stick (since ad87a9fa).
- A saves backup too big for Discord: a long career's DVD and magazine covers are about 1 MB each, so it passes the
  limit even without F12 screenshots. Say so and escalate without it; don't blame screenshots.
- Minimap with no roads (markers only, the world map fine) on Intel graphics: a bug in Intel's graphics driver. Staff
  advise updating the Intel driver; no game fix yet.
- Career, no races left on the map (and nothing on the radar), or parts and cars still locked: the most common post
  of all, and members answer it right. Answer in this order, briefly:
  1. Career Status says what the stage still needs (URL races, sponsor races, cover shoots, the car's stars): T or
     F4 on a keyboard, D-pad Down on a controller, the Career status button (top row) on a touch screen.
  2. Most often it is the car's visual rating (its stars): a stage's sponsor asks for DVD cover shoots, and a shoot
     shows on the map only once the car's visual rating reaches what it needs (checked in the game's code). More
     stars come from visual parts: body kits, paint and vinyls, rims, and the specialties (neons, tint, hydraulics,
     audio). Players who did this got their next races ("upgraded my car to 3 stars and got invited for a special
     event, then a URL race showed"). A shop not found yet is not on the map: drive around the city, the newer areas
     too. Parts and cars unlock as the stage's races are won.
  3. Only then the original game's hidden races: they never show on the map or the radar, only a marker on the street
     when you drive near. Settings, Game, More game settings, Show hidden races (off by default) puts them on the map
     and the radar from the next Play.
  Saves (Saves, Download backup) and escalating only when all three are done and still nothing shows.
- Wide body kits, the most asked question of all (how to get them, locked after finishing the game, more than one
  car): as in the original game, 11 outrun wins in stage 5 put a star on the map before the last URL race (S5_URL_9),
  and the car driven there gets its wide body; the other stages' outruns give unique hoods, spoilers, rims and
  vinyls the same way, one car each. Since 2970e8e, once Caleb is beaten the shops have all of these unique visual parts
  for every car of the career, free (the Body Shop's last category, Wide Body Kits): the original game's ending opens
  them only in Customize mode, whose cars free roam never drives. The developer chose this on 2026-11-02 after the
  forum's "Unlock wide body kits." suggestion. The unique performance parts stay one per stage. Save editors and other
  people's 100 % saves are not needed for it.
- Career, a unique-part text message (the wide body kit, a performance part: "go to the star on the map") but no
  star on the map: the original game, the same on PC, not a bug (the developer checked it on 2026-11-02 and keeps
  it). In stage 5, once the last URL race (S5_URL_9) is won, the map shows only Caleb's race and no stars, but the
  message still arrives at the 6th and 11th stage-5 outrun win. Since 2970e8e, beating Caleb puts every unique visual
  part in the shops for every car (above), so that is the answer; the stage-5 unique performance part is only on an
  earlier save (cloud saves' earlier copy, or a backup). The wide body star never shows while driving an SUV (Hummer,
  Navigator, Escalade): switch cars. Show hidden races does not change any of this (stars are not races).
- The game's outrun challengers (the tuners, grey arrows on the minimap) gone from the whole city, online or not: the
  original game's limit, not a bug. Each stage offers only so many outruns: none once the stage's outrun wins reach 10
  in stage 2, 15 in stage 3 and 20 in stages 4 and 5 (checked in the game's code), and none while a unique part's
  star waits on the map. They come back with the next stage; a poster in stage 5 had them again once Caleb was beaten
  (thread 1566761182046716045, solved by the poster). Online, also by design and only for a while: none by a race
  lobby's spot or at a shop's or garage's door, and none during an outrun with another player; they come back as you
  drive on. Ask the stage and point to Career Status (T or F4); escalate only none anywhere below the stage's limit,
  with the log.
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

## Known open problems (as of 2026-11-03)

The developer updates this list. Check the commits after that date before calling one open: a fix may have come. The
context lists every problem the developer has a report on, with its first thread: point posters there, and set
`duplicate_of` to it, rather than searching for "the main thread". Where an entry names a thread, link that one.

- Free roam since the release of 2 November, 22:47 UTC: the game freezes (`[watchdog] no Present`) or stops with
  error 134 among many players, often right after another player's car opens or closes its hood, trunk or doors. The
  logs stop in sub_00433ad0 < sub_00433d10 (a freeze's stack may start at sub_00433d10). Fixed on 3 November
  (bf4fe9f): a car that came into a slot after a car of another type shut a part that was still moving. Until that is
  live, no workaround is known; once it is, reload the page (Ctrl+F5 on a PC), and a log from the new version is a new
  report. Thread 1566886167629144215. Not the error 134 in garages, car lots, cover shoots and race lobbies of 2
  November (sub_004380f0 < sub_00438140), which is fixed and live since that release.
- iPhone and iPad: the browser closes the game for memory, and the start page comes back (worst on 4 GB iPhones, 12
  to 14, but newer ones too); some logs end in `GPU-LOST` instead. Several improvements are live; it still happens.
  No setting fixes it. Ask for the log from the start page right after it happened, and point to its thread,
  1564554732649971802; escalate only a log on the live version that adds something.
  Kicked out every 15 to 20 minutes on an iPhone is this problem too: say so and link the thread in the first reply.
- iPhone Safari: quick repeated taps (the touch menus' arrows) zoom the page on iOS 26.6 and 27, although the
  26 October fix against zooming is live. No fix yet.
- Huawei phones and tablets with Maleoon graphics: the unsafe-webgpu flag lets the game start, then the graphics
  stop (`GPU-LOST`) within half a minute to a few minutes; frame rate 30 does not help. No fix; thread
  1564433667739222136 (not 1564698358403301446, an AMD PC's).
- Android phones with 2 to 4 GB of memory: the game does not start, the browser cannot reserve its memory ("could
  not allocate memory" in the log) on every Play. No fix; thread 1564275806937682000.
- Event Results after a career race from free roam ignoring Enter and a click on Continue (keyboard, 3 November):
  plugging in a controller got a player past it. Ask for the log while it is stuck (F9, Download log).
- Phones with Adreno 6xx graphics (Adreno 610 among them, often Android 10 or 11): `GPU-LOST` at the first frame, black
  screen, every Play; the log's `[gpu] device lost` line says `VK_ERROR_FORMAT_NOT_SUPPORTED` "While checking external
  image import support with TextureFormat::RGBA8Unorm". Chrome asks the phone's graphics driver whether it may use the
  picture it made for the page, the driver says no, and Chrome takes that for a lost device: no fix on the game's side,
  no setting or flag helps; a system update for the phone may. Since 3 November (pending release) the page says so
  itself (`GPU-ANDROID-DRIVER`). Other Adreno 6xx phones play fine. First thread 1565320939904704613.
- Phones with PowerVR BXM graphics (MediaTek Dimensity 7020/7025): the graphics stop within seconds in landscape
  (`VK_ERROR_OUT_OF_DEVICE_MEMORY` in the log); portrait works for some.
- Black picture with sound on Intel graphics of the 9th generation (HD 510 and similar), in Chrome and Firefox: the
  log shows the game drawing with no WebGPU errors. No fix.
- Free roam: the world's loading screen, or roads missing, while driving among many players (seconds on a PC, half a
  minute on a phone; once it never ended). Cause found on 29 October: the game loads no part of the city while it
  loads a player's car, and with players coming and going their cars load one after another, so the city waited for
  all of them. Fixed on 29 October: players' cars now wait while the city loads. Until that version is live, Players'
  cars shown at 16 (More online settings) shortens the wait; a screen that never ends needs a reload of the page.
- Free roam: friends' cars stop coming in for good (the log: "did not load in 16 s" again and again for minutes), then
  leaving free roam hangs on the loading screen. One log so far (Chrome on Linux, 2 November): one read of the game's
  files went into the page and never came back, and every read after it waited (the watchdog: thread 3 in ReadFile
  for minutes). Since 3 November (pending release) a cache file that does not open in 10 s is skipped (its reads come
  from the server), and the log says `[io] … unanswered for N s` with the file: a log with such a line is new, escalate
  it. Not this: "the car loader has been busy for N s" where the frame numbers barely moved (the tab was in the
  background: the game waits there).
- Back to free roam after a race with other players: endless loading. Two causes are fixed: the crash on the way back
  (`INVALID FREE` and a `TRAP` after the lobby screen, 39f2ca8, 26 October) and the trip stopping after the crib's
  save went to the main menu (db3f8b5, 27 October). A report on a version with both is new: escalate it with the log.
- Sound on a PC that goes silent after the game was paused or its window inactive (`the AudioWorklet stopped
  consuming` in the log, on the live version). On phones, sound that chopped up got a fix on 26 October: a report
  of that after it is new.
- Radio with a music folder: songs played slowed down and an octave low where the game mixes its sound at 22050 Hz
  (machines the game rates as slow). Fixed on 28 October (492b451): on a version with it, a report of slow songs is
  new; ask for the log after a few songs, and the files' type (mp3, flac, ...).
- Radio with a music folder: the game crashed (exit code 134, `sub_0072dcf8` or `sub_0072ad58` at the top of the
  stack) as one song ended and Radio started the next: in the menus after an outrun or a race, or on the trip to a
  race. Fixed on 2 November (fbe1cef9), live since that afternoon; Radio no longer needs to be off. On a version with
  it, a report of this crash is new: escalate it with the log.
- A controller's right stick looking round the car (live since the morning of 3 November): seen from the side or the
  front the car has no front wheels (looking back, no rear ones), its reflections are wrong, "things not loaded".
  That is how it is drawn, not loading: no log needed. Fixed on 3 November (77a2a706), which also makes the stick
  straight down the game's own Look Back; until that is live, B (Look Back) shows the rear view whole. Thread
  1567127501359153282.

## Decided

The developer said these in public; repeat them briefly and kindly when a post asks for exactly one, otherwise none:

- Other games (Most Wanted, others): this project is Underground 2, and the developer has announced nothing else.
  Never say another game will or won't come, and never point to other sites that run one.
- No text or voice chat in free roam: it needs moderation. Quick chat (1-4, then 1-4) is there instead. The game's
  own Online mode keeps its Lobby and game-room chat, with a slur filter (the guide's section 7).
- No cloud streaming of the game: too complex and too expensive.
- No music hosted on the server. Music from the player's own device is there: Radio in the page menu (F9) plays a
  folder of their own songs in place of the game's (the guide's section 4).
- No account system; cloud saves (a passphrase) sync saves between browsers.
- Free roam stays one world, not split into sessions.
- The game's own mechanics stay, wall riding in races included (next section).

## What changes and what doesn't

The game plays as the original did. Additions and improvements around it are welcome: multiplayer features, quality of
life, controls, the page, languages, optional graphics settings (off by default: the original look stays the default),
fixes of the original game's crashes and glitches. Changing its core gameplay mechanics is not done: handling and physics, car
performance and tuning, AI and traffic, the career's progression, races and rewards, the economy.

- A suggestion of an addition or improvement: none (the developer reads them), unless it exists already: then say where
  (Power Units in Options, Gameplay; drift mode with X, and since 19f84c9 with the drift races' handling and the car's
  own Drift tuning from Performance Tuning / Dyno; wheels; a controller's right stick looks round the car while driving
  (right looks right; straight down is the game's Look Back since 77a2a706) and turns it in the garage; quick chat,
  with phrases to pick in Settings, Online;
  ghost mode in F9; Upscaled HUD in Settings, Display, more: Menus, HUD and maps (Upscaled), which since 09e907f1
  also shows JeansBig's Detailed Map: the minimap's, world map's and race menus' maps sharp, with the missing
  shortcuts; photo mode with F12 or the pause menu's
  Photo Mode, since a794718; Radio in F9 for the player's own music; a garage of 10 cars, 2 slots more each stage, and
  Trade Car on the crib's car list, since 8d3d149; Exposure and White Level for a picture too bright for the eyes, in
  the game's Options, Display and the pause menu's, since 2619bf7; hidden races on the map, in Settings, Game, more:
  Show hidden races, since dec0e11; wide body kits and the other unique visual parts for every car once Caleb is beaten,
  since 2970e8e; opening the trunk, hood and doors parked in free roam, O (then again: the hood, the doors, all closed)
  or a controller's D-pad down held a second, other players see them, they close when the car drives off, since
  1ccb47ac (the open trunk showed no trunk audio: empty boxes on the player's own car, nothing on others'; since
  68128859 it shows the layout, speakers and amps on every car); car meets on the world map, with the GPS to them,
  since 109bdcc4; Songs in Settings, Game (Uncensored or the game's Original versions) and F7 for the next song,
  since 32540c78; drift battles between players (drift mode on, then the challenge offered as for an outrun: 90 s,
  most drift points win $100 out of the loser's bank, no nitrous, the cars drive through each other), since 19dc8394;
  the guide
  lists them all). A feature named with a commit is there only once the live version has that commit.
- A suggestion that would change a core mechanic: you may answer, briefly and kindly, that the project keeps the
  original game's mechanics as they are, so it won't change that. Only when it clearly is one; otherwise none.
- A "bug" that is how the original game behaves (drift races have no music; a part's info screen; hidden races): say so
  when you are sure, or when the guide or the code shows it; otherwise none.

## Limits players hit (true as of these notes; check the guide and the code)

- No WebGPU, no game: Firefox for Android (play in Chrome), Samsung TV browsers, Windows PCs without Direct3D 12
  (Intel HD 4000 and older; NVIDIA cards whose last driver is 391.35, such as older GT 730s: Chrome offers them only
  its D3D11 backend). Chrome on Linux needs the Vulkan driver. Players report that the Xbox's browser does not run it
  either.
- CPUs without SSE4.1 (AMD Phenom II and older): browsers turn WebAssembly SIMD off ("WebAssembly SIMD unsupported";
  Firefox: "v128 not enabled"), and the game is built with it. No build without it.
- Samsung Xclipse graphics (some Galaxy phones; `GPU-ADAPTER-ANDROID`, chrome://gpu says "Limited support / testing
  currently available on Android"): Chrome blocklists them, and chrome://flags/#enable-unsafe-webgpu, then restarting
  Chrome, lifts that. A player's Xclipse 540 (Android 16; the Exynos 1580 in the Galaxy A56) runs the game with it
  (2026-10-26); other Xclipse chips are not tried yet. On the phone: type the address into Chrome's address bar, or
  open chrome://flags and search for "Unsafe WebGPU". Don't tell players to set it back to Default afterwards: it
  doesn't matter. The flag is worth a try for any `GPU-ADAPTER-ANDROID` (the page says so since 0583ca5), not only
  Xclipse.
- iPhone and iPad need iOS 26 or iPadOS 26 (iPhone 11 and newer can update). On iOS 18 with Safari's WebGPU
  feature flag on, older pages said Ready, then Play showed nothing or "Did not start" with "createWritable is not a
  function"; the page now says `STORAGE-OLD` there. It still can't run there. Say it once; don't change that answer
  later in the thread.
- iPhone: a web page cannot go full screen. Adding the page to the Home Screen (Share menu) opens it without the
  browser's bars. That copy has its own saves: cloud saves bring them over. Only controllers iOS knows (Xbox,
  PlayStation, Switch, MFi) reach the page.
- Wheels: no force feedback in any browser; the clutch does nothing (the game has none).
- Stuttering cars in races with other players: the game's updates go through the player who created the race (the
  other players' cars also come straight from their games, small corrections smoothed), so the one with the steadiest
  connection should create it, and keep the game's tab in front. A connection that stops for a second or more still
  makes cars jump when it comes back.
- One game per browser: a second tab shows "Open elsewhere".

## Don't

- Don't tell players to set runtime variables (`NFSU2_...`) in settings.ini: most do work there, but they are the
  developer's test switches, and a wrong one can break the game. `?env=` links work only on a development server.
  Don't send them to `chrome://flags` unless the guide says so for their case.
- Don't suggest mods, trainers, cheats, save downloads, other builds or other sites.
- Don't answer questions about moderation, bans, kicks, names, donations, Discord channels, server load or the
  server's setup: staff answer those (none).
- Don't guess at release plans or say what the developer will do, beyond the decisions above.
- Career progress from free-roam races with other players, since 0bc01ce (before that live version: they don't
  count, only the prize money): a race counts where the player could win it with Solo there (open on their map, or
  a normal race of their current stage not won yet; sponsor and URL races only when open, SUV races in an SUV;
  never a race of a later stage, a story race or one already won). The winner's career gets the game's own win (the
  race won, its career prize, rep, the stage goes on, a stage's final moves to the next stage); an accepted friend
  (Friends list) in the same race who finishes gets it too, and since 8027226 so does a crew-mate (a player in one
  of their crews), strangers never. Only 1st place counts: 2nd or lower leaves the race on the map to win (Solo
  still has it); since 8027226 the note after such a race says "Only a win counts in your career". The lobby panel marks with a star whom
  it counts for ("A win counts in your career"); the words over a lobby and on the world map end in "- career". A
  lobby whose race counts for the player who opened it takes only players up to that race's stage and the opener's
  friends (and crew-mates since 8027226; a later stage's car would win it most of the time); the others see why at the spot, and since 85c39ac no
  other lobby opens at that start while it waits (before that they could open their own beside it; a full lobby still
  lets them). A
  URL event of 2 or 3 heats races them all in one trip, with the career's points. A race that does not count pays
  by the player's stage (1st what a normal race of the stage pays, at least $400; 2nd to 4th three quarters, half,
  a quarter of that, at least $300/$200/$100). A race that counts pays the career's prize or that, whichever is more. Where the game would offer sponsors, the crib asks on the way back
  (Yes signs, No the next offer). A stage final won online plays the stage's movie after the crib's save. When everyone else
  quits a race with the pause menu's Quit (each pays a fee: $75 at stages 1 to 3; since 9048c0e $225 at stage 4 and
  $375 at stage 5, before that $75 at every stage), the one who stayed wins it; a lost connection or a closed tab
  counts for nobody (on the Mac app a quit does not hand over the win). The race card's question has Solo focused; since 85c39ac, with another player's lobby there to
  join, Join is focused instead. Since c9d0a59 a lobby's panel warns a stock car (the game's lowest class) at a stage
  3, 4 or 5 race and a class-2 car at a stage-5 race that the cars there are usually much faster than theirs (in the
  live races such cars won a quarter to half as often as an even field would); they can still join.
  Races of the main menu's Online mode never count. Solo is as it always was.
