# Playing NFS Underground 2 in the browser

The game runs in Google Chrome and in Firefox. One server hosts it: it keeps the game files, serves the game to the
browsers and connects players for free roam. On a Mac it serves that Mac; for other PCs it goes behind HTTPS, usually
on a VPS ([VPS.md](VPS.md)). Nothing is installed on the players' PCs, and no file is picked by hand.

## 1. Start it (on the Mac)

```sh
web/start.sh            # or: cd web && npm start
```

- The first start takes longer: it installs the page's tools and builds what is missing. Later starts go straight
  to the server, unless the code changed.
- It looks for the game in `~/projects/nsfu2/Need for Speed - Underground 2`. If the game is somewhere else, start
  once with `web/start.sh --data "/path/to/the folder with SPEED2.EXE"`. The folder is remembered.
- The game folder is only read, never changed.
- It prints the address to open. `Ctrl-C` stops the server.

| Option | What it does |
|---|---|
| `--open` | Also opens the game in Chrome on this Mac. |
| `--http-port 8080` | Use another port, if this one is taken. |
| `--no-build` | Serve what is there, without checking the build. |

On the Mac itself, open **http://localhost:8080/** and press **Play** (or Enter).

The start page shows the download, then **Ready**. Play is on once the game module has loaded, which takes a second
or two. The page shows a problem only when there is one, with what to do about it.

## 2. Other PCs

The browser runs the game only on a secure page (`https://`, or `localhost` on the machine itself), so friends play
from a server behind HTTPS: a certificate on the server, or a proxy or tunnel in front of it. [VPS.md](VPS.md) has
the ways to set that up. Each player then opens that `https://` address in the latest **Google Chrome** or
**Firefox** and presses **Play**.

The first start on a PC downloads about 7 MB of game code and, while playing, the game data it needs, compressed on
the way. That PC keeps the data, so later starts are quick. To load it up front, press **Preload the game data** in
Game data on this PC, at the bottom of the start page: 2 GB (all but the movies), about 1.4 GB over the network.
After that no loading screen waits on the network, which matters on a slow line to a remote server. The movies
stream from the server as they play; with Movies off (Settings, Game) the game never asks for them.

**Offline:** a PC that has opened the page once also starts the game when the server cannot be reached (no
network, or the server is down): the page keeps its own copy of itself and of the game code, updated each time the
server answers. The start page then says **Offline**, and the game runs from the game data on that PC, so download
it first. The movies are off for such a game unless every one of them is on the PC already. Free roam and Online
need the server; your saves are in the browser either way.

## 3. Controls

| Key or button | What it does |
|---|---|
| Arrows or W A S D | Throttle, brake and reverse, steer |
| Alt | Nitrous |
| Space | Handbrake |
| Shift, Ctrl | Gear up, gear down (manual gearbox) |
| C, B | Camera, look back |
| R | Respawn |
| H, J | Hydraulics, with the Hydraulics part (Car Specialties): hold H to pressurize or J to bounce, then the arrows pick the corners; under 10 mph |
| N | Horn (free roam and races with other players) |
| M, Tab | Map, messages |
| Esc or P | Pause; Esc also goes back in menus |
| Enter | Choose (menus); in free roam, start an event at its marker or take an outrun |
| F12 | The game's screenshot, kept with your saves |
| **F8** | Frame-rate counter: off, on, detailed |
| **F9** | The page menu: when the game last saved, full screen, counter, controls, log, **Quit game** |
| **Hold Esc** | Leave full screen (a short press still goes to the game) |

The start page shows these keys under **Controls**, and F9, Controls shows them while you play.

- **Change the keys:** in the game, Options, Controls. The horn stays on N (the game has no horn of its own).
- **Mouse:** works in the menus.
- **Controller:** an Xbox or PlayStation pad works through the browser. The browser shows a pad to the page only
  after a button is pressed on it. A click of the left stick is the horn, unless your car has hydraulics: then the
  stick clicks are the game's hydraulics (left: pressurize, right: bounce).
- **Rumble:** a controller that can rumble in Chrome (Xbox, DualSense and others) shakes when you crash, scrape a
  wall or land hard. Settings, Controller, Rumble turns it off. Firefox has no rumble.
- **Sound** starts with the Play click. If the browser still keeps it off, a note says so, and a click on the game or
  a key press turns it on. A controller button cannot: the browser does not count it.
- **Firefox:** Esc leaves full screen at once (Firefox has no keyboard lock); in full screen, pause with P.
- **Quit** from the game's main menu, or with F9, Quit game. Both let the game save and close. If a loading screen
  ignores the request, the same menu item then reads "Close the game now". As a last resort it reads "Stop the
  game". Closing the tab asks first, because a save in progress could be cut short.

## 4. Settings

**Online** (under Saves) and **Settings** on the start page hold this browser's `settings.ini`, the same file and
keys as the Mac app. Each change is saved at once and applies the next time you press Play.

| Group | Settings |
|---|---|
| Online | its own section under Saves: how many play on this server now, free roam with others, your name (in free roam and online), room, the game's Online mode (on by default); more: solid cars and the free-roam network |
| Game | movies, autosave; more: startup movies (skippable, original or off), sound, pause or keep running in the background, sound output latency |
| Display | full screen on Play (off: the game plays in the browser window), resolution scale, frame rate (auto follows the screen: 120 on a 120 Hz display, 240 on a 240 Hz one; 60, 120 or 240; Online and LAN races at most 120); more: HUD layout, movie framing, cutscene bars, other cars' headlights (players, all cars, off), frame-rate counter |
| Controller | rumble; more: controller on or off, menu buttons, dead zones |

- **More settings:** each group's "More … settings" has the less common ones, and says how many of them you
  changed. **Reset** next to a changed setting puts its default back.
- **Edit the file:** "Edit settings.ini as text" opens the file itself. A line the game would ignore is shown with
  the reason. A line that sets a runtime variable by its own name (`NFSU2_ROAM = 1`) shows on the row it belongs to,
  and every such line is listed under "Other variables in this file". Lines that would name a file of your saves or
  drive the game by itself (`NFSU2_REGFILE`, `NFSU2_INPUT_SCRIPT`, ...) are ignored: only the page sets those.
- **Links:** settings in a page address (`?env=...`) work only on a development server (one started with `--dev`).
  On yours a link cannot change your game; the start page says so when an address carries them.
- **The look:** the game looks as it did originally. Any graphics enhancement will be a separate setting, off
  unless you turn it on.
- **Two tabs:** one browser runs one game at a time. A second tab shows "Open elsewhere" and its Settings are
  read only. It takes over when the first tab closes.

## 5. Saves

- **Where they are:** your profiles and career are saved **in this browser, on this PC** (the site storage for
  the server's address). They are not on the server. Each address keeps its own save store, so stick to one.
- **Autosave:** the game saves your progress by itself after races, purchases and changed options. The game starts
  every profile on Save = Prompt, whose "save?" question opens on No, so Enter through the results skips the save;
  the page sets each profile to Autosave once (the game's Options, Gameplay, Save), at the first Play after the
  profile was saved. Pick Prompt or Off there and it stays. Settings, Game, Autosave off leaves the game's own setting
  alone.
- **When it saved:** "Progress saved" shows over the game each time it saves your profile, F9 shows the last save, and
  after the game closes the start page says when it last saved (or that nothing was saved during that game).
- **Rename:** Rename next to a profile in Saves gives it another name: letters and digits, up to 7, as the game's
  own name field takes them. Its career, cars and DVD and magazine covers go with it. Quit the game first. When
  "Your name" is empty, the other players see the new name.
- **Backup:** Saves, Download a backup gives one zip with your profiles, the game's options (`registry.txt`),
  `settings.ini` and F12 screenshots.
- **Restore:** Saves, Restore a backup puts a zip back, on this PC or on another one. It replaces what this browser
  has, after showing what is in the zip, including what its `settings.ini` would change on the network (free roam
  on, the name others see). A very large backup keeps every save and settings file. Only screenshots past 512 MB
  are left out. Restore only backups you made yourself or trust.
- **Backups are your safety net.** When the disk is nearly full, the browser may clear a site's storage unless it
  promised to keep it. Chrome decides that by itself, and the page cannot see how full the disk is, so it does not warn.
  Firefox asks by the address bar (the Saves section says so while it asks): allow it. Make a backup now and then.
- **Preloading the game data:** "Preload the game data" in Game data on this PC fetches the game folder but the
  movies into this browser, compressed on the way, with its progress and the time left. You can play meanwhile:
  preloading pauses while the game runs and goes on when it ends. Stop it whenever you like; what came is kept, and
  "Preload the rest" goes on from there, also after a reload. It needs about 2 GB of free disk space. The page says
  so before it starts when the browser has less room.
- **Game data on this PC:** "Delete the game data on this PC" frees disk space on this PC. Your saves stay; the game
  loads the data from the server again when it needs it.

## 6. Free roam with other players

Free roam is shared with **everyone** on the server who turns it on. Use a Room to keep to a group.

1. Each player turns on **Online, Free roam with others** on the start page. The first Play asks too (Turn on or
   Not now, and Esc is Not now); it asks once, and the switch changes the answer later. **Online, Your name** is
   the name the others see (empty: your profile name); the question shows it once there is one, to change.
2. Everyone plays from the same server's address.
3. Each player starts or loads a career and drives in free roam. Players find each other by themselves, and their
   cars appear in your city with their names over them, as in the game's own LAN races.

Good to know:

- **How many:** up to 2000 players on a server set up as VPS.md describes, 128 on one started by hand (its owner can
  change that). Only free roam and the LAN and Online screens take a place; single-player takes none. When it is full,
  the game says so and lets you in by itself when a place frees up. The server pairs each player with the 32 players
  nearest to them, and your game shows all 32 of their cars at once. Their names show over the cars within 100 m, and
  each gets an arrow on the minimap when it is near and a marker on the world map (M), as the game's own opponents
  do; the world map shows where they were when you opened it. A player in a race, a shop or the menus is not in your
  free roam: no car, no arrow, no marker until they are back. A player farther than 300 m sends
  you their car 5 times a second instead of 20 (past 1 km, twice), which saves traffic and doesn't show at that
  distance. When many players arrive at once, their cars load one after another, about two a second.
- **Solid cars:** you collide with the other players' cars (Online, More online settings,
  Solid cars; off: you drive through them). A car that appears on top of yours, at a garage or after a jump, lets you
  through until you are apart. Each player's own game moves their car: a car you hit is pushed in its owner's game,
  and moves on your screen as it does there.
- **Names over the cars** follow the game's own player-names option, as in its LAN races. A player who paused shows
  as "paused" there.
- **Horn:** hold N (or click the controller's left stick) to honk. The others hear it from your car: louder the nearer
  they are, from the side your car is on, and lower in pitch as you drive away from them. Each player's horn has its
  own pitch, so you can tell who honks. It sounds while the game runs, not in the pause menu or on the map. It works
  in Online and LAN races too.
- **Hydraulics and nitrous:** the others see your car lift, tilt and bounce on its hydraulics, with its wheels on the
  road, and your nitrous as the game draws it on any car: the light trails behind it. They hear them from your car
  with the game's own sounds, as you hear your own: the nitrous going on and off, letting it out while standing or
  off the throttle, each wheel's pump and valve and a landing, louder the nearer they are. A player on an older
  version sees the body move but not the wheels, and no nitrous, and hears none of it.
- **Headlights:** the other players' headlights light the road in front of their cars, as yours do, a little dimmer
  and in their own colour; in Online and LAN races too. Display, more: Other cars' headlights (All cars: AI racers and
  traffic too; Off: only yours, as in the original game). The Windows plugin draws only your own.
- **Outruns:** stay within about 30 m of another player for 3 seconds and the game offers an outrun, as it does with
  its own tuners: the outrun icon lights up and the top of the screen says "Press Enter to challenge ... to an
  outrun" (on a controller, D-pad up, the game's Engage). The other player sees "... challenges you to an outrun" and
  takes it with Enter as well, or lets it go: a challenge lapses after 15 seconds, or when you are more than 60 m
  apart. Then both games run the game's own outrun against each other's car: its "Feet" meter, and whoever gets
  1,000 feet (300 m) ahead of the other wins, and the bank pays or charges as in an outrun against a tuner. Leaving it
  (back to the menus, quitting, losing the connection) loses it. The other players' cars stay, and they see the two
  drive. Players on the Windows plugin are not offered outruns.
- **Rooms:** set the same "Room" to see only some of the players' cars. A room is a filter between players, not
  privacy: every browser on the server still sees your name ("Your name", or the profile name), whether you are in
  the menus or driving, and the room, and someone with their own tools could join it. Play on a server only with
  people you would show your name and car to.
- **Who sees what:** with free roam off (the default), the game does not join the server's network at all. With it
  on, everyone on the server sees your name and status, and players in free roam with you see your car and where it
  is. The server reads where your car is, to pick the players near you, and keeps nothing of it. Nobody sees your
  real address: the server gives each player a virtual one (`10.77.x.x`).
- **A tab in the background** (or another window in front) pauses your game. The others see you as paused: your car stays where you stopped, with "paused" after your name, until you come back.
- **Closing the tab:** the others see your car disappear after a few seconds.
- **Messages over the game** say when a player joins free roam, goes into a race, is back in free roam after it, or
  leaves. The menus and the shops are not announced.
- **The game's own LAN races** (its LAN menu) work between players on the same server too, with free roam on or off.

## 7. The game's Online mode

The game's own **Online** menu works again, against this server instead of EA's long-gone one: everyone who plays
from the same server address can meet there.

1. Main menu, **Online**. You go straight to the Online Main Menu under your name: **Online, Your name** on the
   start page, or your profile name when that is empty. There are no EA accounts, so the game's EA Login screens are
   skipped.
2. **Lobby** shows the games. **Create Game** (race type, track, options; Game Type Unranked), or **Refresh** and
   **Join Game**. In the game room, chat, press **Ready**; the creator presses **Start Race**. When only some
   players are ready, Start Race counts down 10 seconds: whoever is still not ready then is removed from the game,
   and the race starts without them.

Good to know:

- **Your name online** keeps letters, digits, `-` and `_` (at most 15): `Thom K.` plays as `Thom_K`. A changed name
  applies the next time you press Play.
- **A name that reads as a slur** is not taken: Settings does not save it as **Your name**, the Online mode
  refuses it, and in free roam the other players see `Player` over your car. Digits or signs for letters (`1` for
  `i`), gaps and repeated letters read as the word they spell; a number (a year, two digits at the end) is a number
  (`Hakan1995` and `Dani99` are names).
- **Your name is yours while you are online**, and while you race. Someone else can use it when you are not; the
  rankings and your EA Messenger friends list follow the name, so whoever plays under it while you are away sees your
  friends list and can change it. If another player is online under it, you play under it with four digits in place
  of its end (`NAME` becomes `NAME4821`), usually the same digits when you come back. Set **Online, Your name** to
  keep your own.
- **No name and no profile:** the game's own EA Login screens come instead. **Use Existing EA Account**, any name
  (3 to 15 letters, digits, `-` or `_`) and any password, **Continue**; **No** to "create a profile"; your persona.
  Nothing is checked and nothing is kept: there are no accounts. Type a password you use nowhere else: the game
  sends it to the server only lightly scrambled (the server never reads it).
- **Kept on disk:** only race results per name (Rankings) and your EA Messenger friends list (names). Account
  passwords are never read. Game passwords are checked using a digest held in memory for that game, never logged
  or saved to disk. Players you block stay blocked while you are online; the block is not kept after that.
- **Others see** your name, your game, your chat and your results, never your real address.
- **Every track is open** when you create a game, either direction, whatever your career has unlocked, and a new
  game starts at **2 laps** (the game's own default was 3; Options, Laps changes it). The same goes for the game's
  LAN races. The career and Quick Race keep their unlocks.
- **Up to 6 players** race in one game (Options, Max Players; the game's own most was 4, and 6 only in Street X).
  Drag keeps one car per lane: 4, or 3 on the track with three lanes.
- **Horn:** N (or the controller's left stick click) honks in a race too, as in free roam: the others hear it from your
  car. The same in the game's LAN races.
- **The Lobby lists every open game**, whatever its race type and options (Collision Detection, Race Breaker, Car
  Performance Restrictions, Ranked). The lobby's **Filter** only puts the games it asks for first; in the original
  game it hid all others, so a game made with Collision Detection off was seen by no one. A game with **Private
  Room** on is not listed, as in the original game.
- **Ranked games** need three players at least. Ranked wins give ranking points.
- **A race counts** in Rankings once two of its players' games sent the same result (every game sends its own at
  the results screen), so a race that only one player finished does not count. Each player races from their own
  browser.
- **EA Messenger:** Search finds players on this server; Send Message works. To become friends, both send a
  friend request (the other's request shows the asker in your list: open them, Send Friend Request).
- **Game passwords:** a password game requires the matching password to join.
- **Not yet:** the points table after a ranked race stays empty (the race still counts in Rankings).
- **Who is on:** Online on the start page shows how many play on this server now, and how many of them are in Online
  mode (in its menus or racing). Only the two numbers: no names.
- Turning it off: Online, Online mode on the start page (the game then cannot connect).

## 8. When something is wrong

| What you see | What to do |
|---|---|
| "Secure, isolated page" on another PC | Open the server's `https://` address, not `http`. Only `localhost` works without HTTPS. |
| "WebGPU" | Update the browser; look at `chrome://gpu` in Chrome, about:support (Graphics) in Firefox. The game needs WebGPU. Firefox has it on Windows and macOS; where it is off, Chrome plays. |
| "WebGPU" in Chrome on Linux, while `chrome://gpu` says WebGPU is hardware accelerated | Most likely the Vulkan driver is missing: Chrome runs WebGPU on Vulkan there (the rest of Chrome on OpenGL). `ls /usr/share/vulkan/icd.d/` lists the installed ones (`intel_icd` for Intel, `radeon_icd` for AMD). Install it with `sudo apt install mesa-vulkan-drivers` on Debian and Ubuntu, `sudo dnf install mesa-vulkan-drivers` on Fedora, `vulkan-intel` or `vulkan-radeon` on Arch, then quit Chrome completely and start it again. If the driver is there, the Dawn Info section further down `chrome://gpu` lists each adapter WebGPU sees and why one is blocked. |
| "WebGPU" with a note about compressed textures | Nothing to do: the game plays. This GPU (in many phones, Pixels among them) cannot read the game's compressed textures, so the page unpacks them as the game loads: about 200 MB more memory, and loading screens may pause for a moment. |
| "Game module" | The game did not load, or the server is not reachable. The page tries again by itself, or press **Retry**. If the module is missing, start the server with `web/start.sh`, which builds it. |
| "A new version of the game is on the server" | Press **Reload**: the page loads the new version, and your saves and settings stay. The log is cleared by the reload, so download it first if you want to keep it. |
| "Open elsewhere" | The game runs in another tab of this browser. Switch to it, or close it. |
| No sound | Click the game or press a key. Check the sound setting and the output latency (try "Robust"). |
| Stuck on a loading screen | F9, Quit game; then "Close the game now"; then "Stop the game". When the log ends in `[data]` lines, the game was waiting for its data from the server: it goes on when the data comes. |
| Anything odd | F9, Download log (or "Download log" on the start page after the game ended) and keep the file. It has the game's own log. Before you post it anywhere: it names your browser, its version and your system, your graphics chip, your time zone, and the names and virtual addresses of the players you met (not the server's address: only whether it was this machine, on the LAN or remote). |
| `Port 8080 is in use` when starting | The server already runs; open its address. Or start on another port: `web/start.sh --http-port 9080`. |
| Full screen | The game plays in the browser window. F9, Full screen goes full screen, and holding Esc leaves it (in Firefox, a press). "Full screen on Play" in Settings starts every game full screen. |
