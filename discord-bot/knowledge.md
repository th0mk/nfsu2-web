# Maintainer's notes

The player's guide (below these notes) is the reference for what the page and the game do. These notes add what the
guide does not say: how the project runs, what a useful report needs, the answers that keep coming up, and what is
known or decided. The code is public: https://github.com/th0mk/nfsu2-web.

## The project

- Players open https://nfsu2.xyz in Chrome, Edge (or another Chromium browser: Brave, Opera) or Firefox (Safari 26 on a
  Mac, iPhone or iPad works too). Chromium browsers run the game as Chrome does: the `BROWSER-CHROMIUM` warning ("Only
  Chrome and Firefox are tested") is harmless, and the release after 371f501 no longer shows it, nor one for Safari.
  The server has the game files: players need no copy of their own and install nothing. Saves stay in
  the browser, or also on the server with cloud saves (a passphrase only the player has; the page called it a keyphrase before 2026-10-29, so players may say either).
- The page updates itself: before a game it loads the new version by itself; during a game it shows "A new version of
  the game is on the server", and Reload or Play loads it. A tab left open runs the old version until then. When a fix
  is live, the answer is when it was fixed, since when it is live, and "reload the page (Ctrl+F5 on a PC)".
- The page's own settings are on the start page, in its tabs (Online; Game, with its Game and Display parts;
  Mods; Controls), not in the game's Options. Players who can't find one are looking in the game: say "on the start
  page", and that it applies at the next Play (F9, Quit game first).
- The FAQ's answers come in the reader's Discord language (Discord's own Settings, Language), and in #help the bot
  replies with one line from the FAQ's answer when a question asks exactly that.
- Multiplayer: free roam with everyone on the server (Online, Free roam with others), the game's Online mode (a name,
  no EA account) and races from free roam or the Online menus, up to 6 players. Crews, rooms, ghost mode (off by
  default: cars collide; F9 switches it), quick chat (1-4, then 1-4; a controller's right stick pressed in, then
  flicked twice; its special phrase Countdown counts 5 to GO! for the players lined up near you), horn (N).
- Split screen (added 2026-11-03, live with the release after that): the main menu's item after Quick Race, 2 to 4
  players on one screen, each on their own controller, or two on one keyboard: Enter joins on the arrows (Right
  Shift nitrous, Right Ctrl or / handbrake, . and , gears, L camera, ; look back), Space on WASD (Left Shift, Space,
  E and Q, C, B); a lone keyboard player has the whole keyboard. Each player has their own view, HUD and minimap.
  Some keyboards drop a key when two players hold many at once. Ported by BirdSentinel (Zack) from mihaits' NFSU2-SplitScreen mod. 3 or 4 players need a
  fast computer (every view is a whole scene). Not with other players over the network.
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
- Sound: browser and system, the output latency setting (start page, Game, More game settings), what is wrong (none, late, crackling).

## The start page and the log, exactly (checked 2026-11-05: web/index.html, web/src/app/settings-ui.ts)

Replies named settings that don't exist ("Settings, Display", "Resolution scale 50% or lower"), left a step out, or
misread the log's numbers:

- Tabs: **Saves, Online, Friends & crews, Game, Mods, Controls**. There is no "Settings" tab, and the game's own
  Options hold none of the page's settings. Game has two parts, Game and Display, and at its end Game data on this
  device (Delete game data) and Edit settings.ini as text; Mods lists every built-in mod with its makers (the mods'
  credits are there, not in the About), its switch or choice where it has a setting, and right after each row's name an (i)
  that opens what it does, who sees it, where it is set and its pages, then Our changes, a row per group (Driving, Free
  roam, Photos and screen, Career and garage, Controls, Saves) whose (i) lists every change in it; Controls has Keyboard
  (Touch on a touch screen), Controller and Site. A page older than the Mods tab has an Other tab with what
  is now Game's end, and the mods' settings in Game.
- A setting behind its part's "More … settings" button needs that step said. Controller (On, Hotplug, Off), the dead
  zones and Trigger full press: Controls, Controller, More controller settings. Sound output latency: Game, More game
  settings. Race HUD, the frame-rate counter: Game, Display, More display settings. The Controller part shows the
  buttons' layout, not the pad's name.
- The mods' settings are on their rows in Mods, named by the mod (no "More" step): Upscaled HUD and Detailed Map
  (Original, Map only, Upscaled), English Subtitles Fixed (off: no movie subtitles in any language), Uncensored &
  Restored Soundtrack (the songs: Uncensored or Original), Show Hidden Races Mod, Visual Reputation Remover. Music in drag
  and drift races is the site's own setting: Game.
- Resolution scale: 50%, 75%, 100%, 150%, 200%. 50% is the lowest (a quarter of the pixels); phones start at 75%.
- Frame rate: Auto, 30, 60, 120, 240. Auto picks one of those from the screen's refresh rate (240 from 239 Hz, 120
  from 119 Hz, 30 at 31 Hz or less, else 60; runtime/framerate.c). A 75 Hz screen gets 60 and a 144-165 Hz one 120,
  and neither spaces those evenly: some frames show twice. No page setting matches such a screen exactly; Auto does not
  "follow the screen" there.
- Cloud saves while the browser syncs: the passphrase box is read-only and Load is hidden. To load another passphrase:
  Stop syncing first (the saves stay), then paste it and press Load.
- The page's menu in the game: F9 on a keyboard, Home on a controller, the ☰ button on a touch screen.
- Frame rate in a log: the frames of a status line divided by its seconds is the real rate; compare it with the limit.
  "GPU done" runs from a frame's submit to its finish, frames in flight included: high while the frame rate is full is
  no slow GPU.
- `[gpu] device lost: … A valid external Instance reference no longer exists` is Chrome's (Dawn's) general text for a
  lost device, on every vendor: no match by itself. What comes before it (a GPU slowing down, canvas resizes, a driver
  hang) tells the cases apart.

## Answers that keep coming up

Checked against the guide or the code, and confirmed by players in threads:

- "It worked yesterday, today it won't start" or a crash on an older version (the log's `game <rev>` is not the live
  one): reload the page, Ctrl+F5 on a PC; when a fix for it is live, say when it was fixed and since when it is live.
- `GPU-ACCEL-OFF`, or Chrome drawing with "Microsoft Basic Render Driver" or SwiftShader: turn on "Use graphics
  acceleration when available" in `chrome://settings/system` and restart Chrome. Moderators answer this often; it works.
- `PAGE-NOT-ISOLATED` ("not cross-origin isolated") on nfsu2.xyz itself: something in the browser changes the page,
  most often an extension. A guest or incognito window works for the players who tried it (its saves are gone when it
  closes: it only shows the cause); then, in a normal window, turn the extensions off one by one.
- Ban boxes (since 2026-11-05). "Someone who played from your network or VPN was banned, so you can't race other
  players from it": someone else's ban names that address (a VPN's server, iCloud Private Relay, a phone network, a
  household); turning the VPN or Private Relay off, or another network, fixes it, and free roam stays open meanwhile.
  "You can't play with others any more" or "You can't race other players…" with "Ban #N": the player's own ban; only
  staff can check or lift it, by that number: ask them to post the number and their in-game name, and never judge
  whether the ban was right. Before 2026-11-05 a VPN user could get a cheater's full ban by address ("turn off the VPN"
  fixed it then too).
- Saves gone after closing the browser: the browser deletes site data when it closes (Chrome: `chrome://settings/content/siteData`,
  allow nfsu2.xyz to save data). Lost saves do not come back; Saves, Download backup, and cloud saves keep a copy.
- `SAVES-UNREADABLE` ("Play without saving?" and "This browser can't read or write your saves right now (…)", on a page
  that has it; before it, "Did not start" with `GAME-START` and `UnknownError: Unknown failure`, the log's `settings.ini
  not read again: UnknownError`): the browser's own storage for nfsu2.xyz answers errors, so the game can neither list
  the profiles nor keep a save; the page writes nothing. Seen in Firefox 159 on a Mac (thread 1567493118897430539),
  still there after a Firefox restart; the game before had `[save] a file of the save folder could not be opened:
  NotFoundError: Entry not found`. Play without saving starts the game anyway, nothing from it kept (the log: `[save]
  playing without saving`). To fix it: restart the computer, and check the disk has free space (Firefox gives a full
  disk the same error). Still there, the page's message says which way. Only where it says the progress "comes back as
  it was at" a time (cloud saves whose last sync went through): first copy or download the passphrase (Cloud saves: the
  eye shows and copies it, the arrow downloads it; Firefox and Safari keep it only in the site's data, so it goes with
  it; Chrome and Edge may also have it in their passwords), then remove the site's data (Firefox: Settings, Privacy &
  Security, Cookies and Site Data, Manage Data, nfsu2.xyz, Remove), reload, and enter the passphrase under Cloud saves.
  Screenshots, covers, this device's settings (graphics, sound, controls), the Put back copy and the downloaded game
  data go with the data, and anything played after that time. Where it says "don't remove
  this site's data" (no cloud saves, or a last sync that did not go through): never suggest removing it; escalate with
  the log. The same words over a running game: the browser refused a file the game saves to, so a save may not have
  been kept; quit, restart the browser, and check the profile's last save on the start page.
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
    false positive, which staff reported to Kaspersky. The start page stays at "Loading the game" in every browser;
    since 701f9358 (3 November, live) it says "Not loading: Your antivirus or network is probably blocking the game"
    after 15 s.
    Adding `nfsu2.xyz` to Kaspersky's trusted addresses, or turning its web protection off, fixed it for several
    players.
  - "This site can't be reached" / the address can't be found (DNS) on a school, university or work network: setting
    the device's DNS to 1.1.1.1 or 9.9.9.9 fixed it.
- Stuck behind a barrier or off the map: R respawns.
- Stuck on the far side of a red locked-area wall (a later stage's area, e.g. Coal Harbor before stage 4), often
  after a hard hit from another player, with R putting the car back there and reloads, the garage and races too
  (the profile kept that spot): fixed on 4 November (0afdbb92), live with the release after that. Then R, and the
  next Play, put the car on the open side of the wall, on the road just before it. Until then nothing in the game
  gets the car out: ask for the log and a backup, and to keep the profile.
- Career free roam with a black world and only the HUD (empty radar, often lower frame rate; menus and quick races
  fine), often after a hard hit from another player: the car was lost off the map and the profile kept that spot, so
  it came back at every Play. Fixed on 1 November (0f17d0a), live since the 2 November release: reload the page and
  Play, and the car starts at the airport car park. On an older page, R respawns.
- Sound late: start page, Game, More game settings, Output latency "Lowest" (the default) is the shortest; Balanced
  and Robust add delay and are for crackling or dropouts. Never suggest Robust for late sound. Bluetooth headphones
  add their own delay.
- The EA Trax songs differ from the original game's (other versions, longer intros, "covers", swear words): they are
  the uncensored versions on purpose: unedited, some in longer album versions (xGMVx's Uncensored & Restored
  Soundtrack). Since 32540c78, start page, Mods, Uncensored & Restored Soundtrack, Original plays them as the game
  shipped them, and F7 (keyboard only) plays the next song.
- A laptop with a graphics card that plays badly or shows a black picture, while its log names the Intel or AMD
  integrated chip: Chrome runs on the wrong one. Windows Settings, System, Display, Graphics: add Chrome, set High
  performance, then quit Chrome completely and start it again. Players confirmed it.
- Touch Nitro or the thumb stick do nothing while Gas and steering work: they act as the controller's buttons, so
  start page, Controls, More controller settings, Controller must be On or Hotplug, not Off. A poster confirmed it.
- Steam Deck or Steam Controller: buttons act as arrow keys, Enter and Esc and no pad shows in Controls, Controller:
  the controller is in its mouse-and-keyboard mode. In Chrome or Edge on a computer, where Controls, Controller has
  Connect a Steam Controller: press it and pick the controller in the browser's list, once (the 2026 controller
  through its dongle confirmed). Otherwise (Firefox, Safari, no such button): add the browser to Steam as a
  non-Steam game with a gamepad layout and start it through Steam (confirmed). The game crawls on a Steam Deck with sound playing: Chrome draws WebGPU on SwiftShader
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
     too. Parts and cars unlock as the stage's races are won. A player who asks for a way round the stars: since
     ad7e4171, start page, Mods, Visual Reputation Remover (off by default, thelink2012's mod) sets the rating by the
     stage, so every DVD cover shoot of the stage is offered at the next free-roam load and pays by that rating; the
     shops then show no Visual Rating box. Offer it as their choice, after the parts.
  3. Only then the original game's hidden races: they never show on the map or the radar, only a marker on the street
     when you drive near. Start page, Mods, Show Hidden Races Mod (off by default) puts them on the map
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
  Navigator, Escalade): switch cars. Show Hidden Races Mod does not change any of this (stars are not races).
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
  `GPU done p50` above 16 ms). Start page, Game, Display, Frame rate 30 is the steady choice for such a phone. To keep 60: the
  lighter graphics. A phone starts with them, but not one where a graphics option was changed before (the log says
  `phone graphics: the graphics options were changed in this browser, they stay`); then the game's Options, Display:
  car and road reflections, motion blur, depth of field, overbright and bleach bypass off, and the resolution on the
  row with a size such as 1916x884 (the screen's own at the page's resolution scale), not 1080p or 1440p, which draw
  more pixels than a phone's default 75%. Players' cars shown (start page, Online) takes 16, 24, 32, 40 or 48
  (since 4 November, pending release: 16, 24, 32, 48 or 64), nothing lower.
- "I picked 64 but see fewer / cars keep popping in and out in busy places": a link went both ways, so a car showed
  only while that player had room for yours too, and most players keep 32, 24 or 16. Since 5 November players get
  the cars of their nearest players one way where those show fewer, and friends' cars load in fewer frames.
- "Cars vanish and come back at a car meet, even close ones": the server ranked the cars round a player by where they
  head and what is in front of the player, which turned with every donut. Since 5 November (pending release) a player
  standing or rolling slowly gets the cars within 60 m first, and a car that came that near stays at least 30 s. A
  player's F9 log says why each car went ("car removed (...: the server: ...)", "car hidden (...)").
- No sound at all on an iPhone: the page played no sound in Silent mode until the release of 4 November (17:02 UTC,
  c75e4c62). Since then it plays as a video does (Safari's audio session "playback"), Silent mode or not: a page
  loaded before that needs a reload (Ctrl+F5 is a PC's: on the iPhone, reload the page). A side effect players
  noticed: other apps' music (Apple Music, Spotify) pauses while the game plays; no setting lets both play.
- "Error code 1" seconds after Play: the game's program file did not load, and the log says why. `HTTP 403` for it:
  a network or filter blocks it (another network, or allow nfsu2.xyz). `no PE signature`: the browser's copy is
  damaged: start page, Game, Delete game data (at the tab's end), reload (Ctrl+F5), Play; saves stay (a poster
  confirmed it).
- A profile's name is the game's: up to 7 letters and digits. Other players see Online, Your name (3 to 15 letters,
  digits, - or _), or the profile's name when that is empty. Saves, Rename renames a profile; the game's main menu,
  Profile Manager, Delete deletes one; to start over, a new profile, or Career, Start New Career.
- An online name is held only once claimed (start page, Online, Your name: Claim, with cloud saves). Unclaimed, it is
  anyone's: "Another player claimed X, so you play as X1234" means someone claimed it first; pick another name, or
  if it is theirs, enter their passphrase under Cloud saves. "Another passphrase claimed X": not the one this browser
  syncs with, so another name. Before f2f3e79 (live 2 November) the page also said so right after the player's own
  claim: reload. Claimed with this browser's passphrase and still given digits on the live version: escalate with
  the log. Never judge whose name it is.
- One name a passphrase, and since the "one name" update (the line "You play as X, your claimed name" under Your
  name) the game always plays as the passphrase's claimed name, whatever Your name says. Before it, a player who
  typed another name after claiming played under that other name, which nobody held: others adding it by name or
  by bumpers got "X has no claimed name, so can't add friends yet", while the player's own requests worked (their
  friends see the claimed name). Saves, Cloud saves shows "This passphrase also holds your claim on X": that is the
  name others can add. To play as the typed name: Change name (once a week; "You can change it on <date>" says when).
  Changing moves the friends along (the start page or the Online menu, whichever comes first).
- Races only with friends, career races too: everyone sets the same uncommon Room (start page, Online) before Play;
  lobbies stay within the room. It is not private. Alone in free roam often means a Room left set: clear it.
- A career without other players: start page, Online, Free roam with others off, then Play. The page menu's "Stop
  waiting: play offline" shows only while the page waits in line for a full server, never during a game.
- Manual or automatic gears: the game's pause menu, Options, Player, Transmission (Quick Race and the Online menu ask
  after the car). Drag races are always manual.
- A player in a bus, fire truck, ambulance or another traffic car: a save editor changed their profile; the game sells
  none. Other cars drive through it. Don't say how.
- The game's data can't go on another drive: the browser keeps it in its own folder for the site. Preloading it is
  optional; start page, Game, Delete game data (at the tab's end) frees the space, and saves stay. "No more room" is
  the browser's limit for the site.
- A profile left out of a restore because "the name inside … is not one the game makes": since a530a5a0 (2 November,
  live that evening) it comes in renamed. Reload, then restore again.

## Known open problems (as of 2026-11-04)

The developer updates this list. Check the commits after that date before calling one open: a fix may have come. The
context lists every problem the developer has a report on, with its first thread: point posters there, and set
`duplicate_of` to it, rather than searching for "the main thread". Where an entry names a thread, link that one.

- Free roam freeze (`[watchdog] no Present`) or error 134 right after another player's car opened or closed its hood,
  trunk or doors (sub_00433ad0 < sub_00433d10): fixed on 3 November (bf4fe9f), live since 14:39 UTC that day. A log on
  an older version: reload the page (Ctrl+F5 on a PC). A newer log that stops there is new: escalate it. Thread
  1566886167629144215. Not the error 134 in garages, car lots, cover shoots and race lobbies of 2
  November (sub_004380f0 < sub_00438140), which is fixed and live since that release.
- iPhone and iPad: the browser closes the game for memory, and the start page comes back (worst on 4 GB iPhones, 12
  to 14, but newer ones too); some logs end in `GPU-LOST` instead. Several improvements are live; it still happens.
  No setting fixes it. Ask for the log from the start page right after it happened, and point to its thread,
  1564554732649971802; escalate only a log on the live version that adds something.
  Kicked out every 15 to 20 minutes on an iPhone is this problem too: say so and link the thread in the first reply.
- iPhone Safari: quick repeated taps (the touch menus' arrows) zoom the page on iOS 26.6 and 27, although the
  26 October fix against zooming is live; still reported on 3 November. No fix yet. Thread 1564469610231758970.
- Huawei phones and tablets with Maleoon graphics: the unsafe-webgpu flag lets the game start, then the graphics
  stop (`GPU-LOST`) within half a minute to a few minutes; frame rate 30 does not help. No fix; thread
  1564433667739222136 (not 1564698358403301446, an AMD PC's).
- Android phones with 2 to 4 GB of memory, and some with more (a 6 GB Pixel 6a): the game does not start, the browser
  cannot reserve its memory ("could not allocate memory" in the log) on every Play. Closing tabs and apps did not
  help the posters who tried: don't suggest it. The log's memory figure is the browser's, rounded down (the 6 GB
  phone shows 4 GB). No fix; thread 1564275806937682000.
- Event Results after a career race from free roam ignoring Enter and a click on Continue (keyboard, 3 November):
  plugging in a controller got a player past it. Also on 4 November in Firefox: the race results stop taking input
  after Alt+Tab and back, not every time (thread 1567493082470158336). No fix yet. Ask for the log while it is stuck
  (F9, Download log), and whether a controller gets past it.
- Phones with Adreno 6xx graphics (Adreno 610 among them, often Android 10 or 11): `GPU-LOST` at the first frame, black
  screen, every Play; the log's `[gpu] device lost` line says `VK_ERROR_FORMAT_NOT_SUPPORTED` "While checking external
  image import support with TextureFormat::RGBA8Unorm". Chrome asks the phone's graphics driver whether it may use the
  picture it made for the page, the driver says no, and Chrome takes that for a lost device: no fix on the game's side,
  no setting or flag helps; a system update for the phone may. Since 3 November (583c516b, live) the page says so
  itself (`GPU-ANDROID-DRIVER`). Other Adreno 6xx phones play fine. First thread 1565320939904704613.
- Phones with PowerVR BXM graphics (MediaTek Dimensity 7020/7025): the graphics stop within seconds in landscape
  (`VK_ERROR_OUT_OF_DEVICE_MEMORY` in the log); portrait works for some.
- Black picture with sound on Intel graphics of the 9th generation (HD 510 and similar), in Chrome and Firefox: the
  log shows the game drawing with no WebGPU errors. No fix.
- Free roam: error 134 after an hour or more among many players, with trips out of free roam and back (the crib,
  shops, cover shoots and special events, races): the log has `[heap] HeapAlloc(...) FAILED` lines before the `TRAP`
  (in tlsf_alloc < gbuf_alloc while friends' cars load, or in sub_00401aa0 on a race's loading screen). The game's
  512 MB of memory runs short on the way back into a crowded free roam: its freed memory came back in pieces too
  small for its next big load (better since d28b0b03), and the textures the game had let go waited to be released
  while the friends' cars' new ones came in (since 8ddeacb8 they go within a few frames: the peak on a trip back
  50-80 MB lower in tests). Since 8ddad1c9 a block freed twice no longer damages the game's memory, and since
  216569ab a texture the memory has no room for no longer writes over other memory (`[com] no memory for ... Lock
  buffer` and `scratch area` lines: the memory was that full). All only once the live version has those commits; no
  setting avoids it for sure before. With them the log says what holds the memory (`[heap] no room for ...` lines)
  and names anything freed twice (`DOUBLE FREE` lines): escalate such a log.
  Since 4 November (pending release) a computer's browser gives the game 768 MB more of that memory, 1.25 GB in all
  (Chrome, Edge or Firefox; not Safari, phones or tablets, nor a computer whose browser says it has less than 8 GB):
  its log then says `[memory] 2048 MB` and `[heap] window above the runtime`. A computer's log with those lines and
  still these failures: escalate it. `[heap] no window above the runtime` means the browser did not give the 2 GB
  (the `[memory]` line says what it refused), and the game runs with 512 MB as before. A settings.ini line
  `NFSU2_HEAP_WINDOW = 0` turns the extra memory off.
- Free roam: the world's loading screen, or roads missing, while driving among many players (seconds on a PC, half a
  minute on a phone; once it never ended). Cause found on 29 October: the game loads no part of the city while it
  loads a player's car, and with players coming and going their cars load one after another, so the city waited for
  all of them. Fixed on 29 October (6db6e279), live: players' cars now wait while the city loads. On a version with
  it, a loading screen that never ends is new: escalate it with the log.
- Free roam: friends' cars stop coming in for good (the log: "did not load in 16 s" again and again for minutes), then
  leaving free roam hangs on the loading screen. One log so far (Chrome on Linux, 2 November): one read of the game's
  files went into the page and never came back, and every read after it waited (the watchdog: thread 3 in ReadFile
  for minutes). Since 3 November (f26f718d, live) a cache file that does not open in 10 s is skipped (its reads come
  from the server), and the log says `[io] … unanswered for N s` with the file: a log with such a line is new, escalate
  it. Not this: "the car loader has been busy for N s" where the frame numbers barely moved (the tab was in the
  background: the game waits there).
- Back to free roam after a race with other players: endless loading. Two causes are fixed: the crash on the way back
  (`INVALID FREE` and a `TRAP` after the lobby screen, 39f2ca8, 26 October) and the trip stopping after the crib's
  save went to the main menu (db3f8b5, 27 October). A report on a version with both is new: escalate it with the log.
- Free roam: the player's own car loses its brake calipers, exhaust tips, spoiler or another shop part (rims,
  mirrors, roof scoop, trunk audio) after entering any shop or the crib, and it stays gone, in the garage too and after
  a car change, until the page is reloaded. One case was fixed on 30 October (d09e1439); the cause behind the reports
  since (another car, a player's or the game's own traffic, with the same part) was fixed on 4 November (c1cd2de7).
  Until that is live, a reload brings the parts back. On a version with it, a report is new: escalate it with the log
  (its `HIBERNATING` lines). Threads 1565572638766796820 (earlier 1562415335540396032), 1567301375547801650 (a 240SX's
  spoiler), 1565481947482820609, 1562624102483759234.
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
- A controller's right stick looking round the car: missing front wheels (fixed in 77a2a706) and a right stick that
  steers or brakes also turning the view (fixed in c04e9dc0), both live since 3 November. Since c04e9dc0 a right stick
  the game's Joypad Configuration steers or brakes with no longer looks round; the left stick does when the game uses
  none of it. On an older version: reload. On a newer one the report is new. Threads 1567127501359153282,
  1567191097149497415 (also 1567230313292112002, "Option to change view stick on gamepad", which this answers).
  Since 1c0af43d the look can be turned off: the start page's Controls, Site, Free look (a click on its Auto makes it Off): no stick
  looks round, straight back is no Look Back, and the stick straight up no longer switches the headlights (G still
  does); the answer to "make the 360 camera optional" (threads 1567427206119166022, 1567191097149497415).
- Free roam, error 134 with `tlsf_alloc < gheap_alloc_c < gbuf_alloc` at the top of the stack while other players'
  cars load, on PCs with AMD and with Intel graphics, also on the live version; `HeapAlloc(2, 0x0, 1398016) FAILED`
  lines come before it. No fix yet. A log with this stack adds to thread 1567239277035790336.
- Free roam, error 134 on the way back to driving right after a race's results, on the live version. Two stacks so
  far: `sub_005c2430 < sub_005d2850 < sub_005811c0` (after a URL race) and `sub_00464c80 < sub_00463d10 <
  sub_00469160` (after the results, the reward and the profile's save). No fix yet: escalate a log with either.
  Threads 1567376467661164634, 1567378408961343508.
- Linux PCs with NVIDIA graphics, Chrome or Brave: the picture stops within seconds of Play (`GPU-LOST`, or black with
  sound). Logs show `vkAllocateMemory failed with VK_ERROR_OUT_OF_DEVICE_MEMORY` or `Requested allocation size (…) is
  smaller than the image requires` on every frame: maybe not one problem. No fix. Ask for the `chrome://gpu` report.
  Threads 1567135816499859456, 1567350752186470540.
- A controller whose gas does nothing while its other controls work: an original 8BitDo Ultimate 2.4G on Windows (RT
  dead, LT reverse works; the Ultimate 2C works), and a wired pad on a Mac seen as `PS3 GamePad (054c:0268)`. No fix
  yet. Ask for the log and what Controls, Controller shows when the trigger is pressed. Threads
  1567258347940352072, 1566999852288843858.
- Horns (yours and other players') still sounding with the game's Master Volume at 0: horns had only their own slider,
  Horn Volume (the game's Options, Audio, also the pause menu's), 10 % to 100 %. Fixed on 4 November (47f8c0c6): horns
  follow Master Volume as the game's own sounds do, so Master Volume at 0 silences them too; Horn Volume stays 10 % to
  100 % (the developer's choice when it was made). Until that is live, Horn Volume at 10 % is the quietest. Thread
  1567278200134836509.
- Ghost mode Everything: the camera jumped up for a moment as a traffic car, a van most of all, drove through the
  player's car, as if they had collided. Not a collision: the game's camera lifts itself over any car its eye is in,
  and the car that passed through went on through the camera behind; nothing else changed (no slow motion, the car's
  speed kept). Fixed on 4 November (77d744ad): the camera goes through those cars too. Until that is live there is no
  workaround but Ghost mode Players (traffic solid). Thread 1567227790888017980.
- Rebinding: the game's own controls are set in its Options, Controls (Keyboard Configuration, Joypad Configuration).
  The site's own keys and buttons (X drift mode, O trunk/hood/doors, N horn, I message, F add friend, 1-4 quick chat;
  a controller's D-pad down held, left stick click horn, right stick click quick chat) can't be changed there. Before
  abba6770 a game control the player put on one of them fired together with ours (D-pad down bound to something was
  held back a second). Since abba6770 such a key or button is left to the game, ours does nothing on it, and the log
  says so once (`[keys] X is bound to Shift Up in the game: drift mode stays off it`, `[pad] D-pad down is bound to
  ...`). A D-pad down, left stick or right stick still on its default (Career Status, hydraulics) works as before.
  Since a10b03d6 the site's own are set on the start page, Controls, Site: a list of them (drift mode, hood/trunk/
  doors, horn, headlights, free look, quick chat, message, add friend, photo mode, hide HUD, menu, next song,
  frame-rate counter) with each one's key and controller button as caps; click one, then press the new key or button,
  or Off (the free look's cap switches Auto / Off with a click). Keys the game itself uses can't
  be picked, quick chat's 1-4 stay, the menu always keeps a key (and Home or both sticks), and a key the player put a
  game control on says so there. It applies at the next Play; the touch controls' Drift and Horn work whatever is
  picked. Until that is live, only the game's own Controls can be changed.

## Decided

The developer said these in public; repeat them briefly and kindly when a post asks for exactly one, otherwise none:

- Other games (Most Wanted, others): this project is Underground 2, and the developer has announced nothing else.
  Never say another game will or won't come, and never point to other sites that run one.
- No open text or voice chat in free roam: it needs moderation. Quick chat (1-4, then 1-4) is there for everyone;
  typed messages go only between friends (I in free roam, or Message in the start page's Friends). The game's own
  Online mode keeps its Lobby and game-room chat, with a slur filter (the guide's section 7).
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
performance and tuning, AI and traffic, the career's progression, races and rewards, the economy. The Mods tab's mods
that change the career (Show Hidden Races Mod, Visual Reputation Remover) are off by default: the player's choice.

- A suggestion of an addition or improvement: none (the developer reads them), unless it exists already: then say where
  (Power Units in Options, Gameplay; Show Player Names there too since 31cc3384 (also in the pause menu's Options, Gameplay): Off
  hides the names over other players' cars, in free roam and LAN races; the Bank box in career free roam shows only
  for 20 s after the bank changes, 10 s when free roam starts and 8 s after the pause menu, the map or a shop, the
  answer to "hide the Bank"; Jump Camera Off in Options, Gameplay (also the pause menu's; it may take the next race
  or free roam to apply) turns off both the jump camera and the slow-motion camera after a crash, the answer to
  "disable the jump and crash cutscenes" (thread 1567563824121249905); drift mode with X, and since 19f84c9 with the drift races' handling and the car's
  own Drift tuning from Performance Tuning / Dyno; wheels; a controller's right stick looks round the car while driving
  (right looks right; straight down is the game's Look Back since 77a2a706; the left stick when the game's controls
  steer or brake with the right one, since c04e9dc0) and turns it in the garage; quick chat,
  with phrases to pick in the start page's Online;
  ghost mode in F9 or the start page's Online: Players lets other players' cars pass through, and Everything (since
  6f7bb374) also the game's traffic and its tuners, in free roam with others: the answer to "ghost mode for traffic"
  and to disabling or removing the traffic or the tuners (NPCs) in free roam, which stay in the world as in the game
  but no longer get in the way; Upscaled HUD in the start page's Mods: Upscaled HUD and Detailed Map (Upscaled),
  which since 09e907f1
  also shows JeansBig's Detailed Map: the minimap's, world map's and race menus' maps sharp, with the missing
  shortcuts, and since 5988eb2a Map only in the same row shows the Detailed Map alone, with the game's own menus and HUD; the time
  played in this browser in Saves, and the last game's under Play (counted only in this browser, not synced; counting
  began on 26 October and stopped at 2 hours until 31 October, so older browsers show "at least"); Delete all for
  the F12 screenshots in Saves, Screenshots (also the way out of a Download backup too big for the browser: "RangeError", "Blob size"); Quebec in
  Online, Your flag, since 5988eb2a; photo mode with F12 or the pause menu's
  Photo Mode, since a794718; Radio in F9 for the player's own music; a garage of 10 cars, 2 slots more each stage, and
  Trade Car on the crib's car list, since 8d3d149; Exposure and White Level for a picture too bright for the eyes, in
  the game's Options, Display and the pause menu's, since 2619bf7; hidden races on the map, in the start page's Mods:
  Show Hidden Races Mod, since dec0e11; DVD cover shoots without the visual rating's stars, in the start page's Mods:
  Visual Reputation Remover (the rating by the stage; the shops show no Visual Rating box), since ad7e4171; wide body
  kits and the other unique visual parts for every car once Caleb is beaten,
  since 2970e8e; opening the trunk, hood and doors parked in free roam, O or a controller's D-pad down held a second,
  other players see them, they close when the car drives off, since 1ccb47ac (the open trunk showed no trunk audio:
  empty boxes on the player's own car, nothing on others'; since 68128859 it shows the layout, speakers and amps on
  every car); since 1c0af43d one part at a time: O the hood, again the trunk alone, the doors alone, all of them, all closed (the
  answer to "open only the hood" or "only the doors", threads 1567568582660071586, 1567034148458205255); car meets on
  the world map, with the GPS to them, since 109bdcc4; the songs in the start page's Mods: Uncensored & Restored
  Soundtrack (Uncensored or the game's Original versions) and F7 for the next song,
  since 32540c78; drift battles between players (drift mode on, then the challenge offered as for an outrun: 90 s,
  most drift points win $100 out of the loser's bank, no nitrous, the cars drive through each other), since 19dc8394;
  a picture that looks stretched, cars too wide, wheels oval: the original game draws everything about 10 % too wide
  (14 % in the garage); the start page's Game, Display, more: 3D proportions, Corrected gives true proportions and round wheels with
  a little more at the sides (Original, the game's own, stays the default), since 6519964e; textures without
  smoothing, pixelated like dgVoodoo's point sampling: the start page's Game, Display, more: Pixelated textures (off by default;
  on overrides the game's Texture Filtering), since 7f34eaa8; C goes on from the game's four views to the drift
  races' camera in every drive, since 0c930f2f; the Graphics Shop's vinyls have a Special category after Sponsor
  with the game's promo vinyls (Snoop Dogg, Capone, Chingy, The Doors and others; free, no visual rating, stages 2
  and 3), since 84c85172; Quick Race: laps 1 to 127 and up to 5 opponents (drags 3), Lap KO's laps and opponents
  changeable (it starts at 5 and 5), and a race of one lap shows Completed % in place of LAP 1/1, since 81fad175;
  headlights on G: a tap turns them off or back on, held is high beam (brighter, reaching farther) until let go, and
  friends in free roam see it; on a controller the look's stick straight up; another key, or none, in Controls,
  Site; since a64138c4; since 1c0af43d the whole HUD hidden while driving, F10 (again: back), free roam and races, the names and
  quick chat lines over other players' cars with it, the pause menu, map and messages still shown, off again at each
  Play, no touch button; another key, or none, in Controls, Site, Hide HUD (thread 1567596160397344910); the guide
  lists them all). A feature named with a commit is there only once the live version has that commit.
- A suggestion that would change a core mechanic: you may answer, briefly and kindly, that the project keeps the
  original game's mechanics as they are, so it won't change that. Only when it clearly is one; otherwise none.
- A "bug" that is how the original game behaves (drag and drift races have no music, unless the start page's Game, Music in
  drag and drift races is on, since 0dc17b5d; a part's info screen; hidden races): say so when you are sure, or when the guide or the
  code shows it; otherwise none.

## Limits players hit (true as of these notes; check the guide and the code)

- No WebGPU, no game: Firefox for Android (play in Chrome), Samsung TV browsers, Windows PCs without Direct3D 12
  (Intel HD 4000, 4400, 4600 and older; NVIDIA cards whose last driver is 391.35, such as older GT 730s: Chrome offers them only
  its D3D11 backend). Chrome on Linux needs the Vulkan driver. Players report that the Xbox's browser does not run it
  either. Windows 7 and 8 (and older): their last Chrome (109) and Firefox (115 ESR) came before WebGPU.
- CPUs without SSE4.1 (AMD Phenom II, Athlon II, the A4/A6/A8-3000 series "Llano", E- and C-series "Bobcat"; Intel
  Core 2 before Penryn; other AMD A4 models have it): browsers turn WebAssembly SIMD off ("WebAssembly SIMD unsupported";
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
  browser's bars. That copy has its own saves: cloud saves bring them over. Only on an iPhone or iPad: an app added
  from Chrome on Android or a PC shares the browser's saves. Only controllers iOS knows (Xbox,
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
- Don't answer questions about moderation, bans, kicks, renaming or removing someone's name, donations, Discord channels, server load or the
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
