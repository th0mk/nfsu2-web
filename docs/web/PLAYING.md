# Playing NFS Underground 2 in the browser

The game runs in Google Chrome and in Firefox. One server hosts it: it keeps the game files, serves the game to the
browsers and connects players for free roam. On a Mac it serves that Mac; for other PCs it goes behind HTTPS, usually
on a VPS ([VPS.md](VPS.md)). Nothing is installed on the players' PCs, and no file is picked by hand.

## 1. Start it (on the Mac)

```sh
web/start.sh            # or: cd web && npm start
```

- The first start takes longer: it installs the page's tools and builds what is missing (the game module, the page
  and the server; the server needs Rust, from https://rustup.rs). Later starts go straight to the server, unless
  the code changed.
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

**Phones and tablets** (Chrome on Android): a tap is a click, so the game's menus work by touch, two fingers
dragged sideways turn the car in the garage, and a finger dragged up or down on a column of the ECU or Turbo tuning
graph moves its dot. The ☰ button opens the page menu. On a phone the game goes full screen
when you press Play and renders at 75% resolution scale (Settings, Display changes both). After a few minutes of driving the page checks whether the phone keeps its frame rate with GPU
time to spare; if so, it renders at 100% from the next game on that phone (and goes back to 75% if it later falls
behind). This only applies while you leave the resolution scale at its default. A phone also starts with lighter
graphics: car and road reflections, motion blur, depth of field, overbright and bleach bypass are off, which halves
the GPU's work in a race (the game's Options, Display turns them back on). A phone that played before gets them once,
unless a graphics option was changed there. Held sideways, the game fills the screen with the touch controls over it; held upright, it shows
in a 4:5 picture at the top with the ☰ button and the touch controls below it. The screen stays on while the game
runs. Chrome's menu, **Add to Home screen** (or **Install app**), puts the game on the home screen as an app that opens
full screen; on a PC the same makes it an app with its own window.

**Touch controls** show on the game while you drive (races and free roam, in a race from the start line so you can
rev in the countdown) and step aside for menus, the pause menu and free roam's cutscenes:

- **Right thumb:** Gas, Brake (and reverse), Nitro and the handbrake. A finger slides from Gas to Brake.
- **Steering**, switched with the button in the top right corner and kept in this browser: **buttons** (◀ ▶, as the
  arrow keys; the default), a **thumb stick** that appears where the left thumb lands, or **tilt**, the phone held like
  a wheel. With tilt, Brake and the handbrake move under the left thumb, and the button beside the switch sets
  straight ahead.
- **Gears:** with a manual gearbox (the race's Transmission choice), gear down and gear up above the steering.
- **Small buttons** along the top: pause, camera, look back, reset; in free roam also the map, messages, horn, and
  Start (an event, or an outrun).
- **Sizes**, in the Controls sheet's Touch tab and kept in this browser: **Control size** for the touch controls, and
  **Menu button size** for the game's own small menu buttons (Back, Continue and the others at the bottom of every
  menu, and the arrows of the main menu's and Quick Race's choices). Both are **Auto** by default: the touch controls
  at 100% on a phone and up to 125% on a tablet, as far as they fit; the menu buttons big enough for a finger, up to
  200%.
- A keyboard or a controller sends them away until you touch the game again. Android phones buzz on crashes.

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
| X | Drift mode in free roam: drift handling, the drift camera and a drift score (see below) |
| 1 to 4, then 1 to 4 | Quick chat in free roam with other players: a group, then a phrase (see section 6) |
| M, Tab | Map, messages |
| Esc or P | Pause; Esc also goes back in menus |
| Enter | Choose (menus); in free roam, start an event at its marker or take an outrun |
| F12 | Photo mode while you drive (see below), and in it the photo; elsewhere a screenshot. Both are PNGs kept with your saves and shown in Saves on the start page |
| **F8** | Frame-rate counter: off, on, detailed |
| **F9** | The page menu: when the game last saved, full screen, counter, ghost mode and crews (in free roam), controls, log, **Quit game**; on a controller Home, or both sticks pressed in; on a touch screen the ☰ button |
| **Hold Esc** | Leave full screen (a short press still goes to the game) |

The start page shows these keys under **Controls**, and F9, Controls shows them while you play.

- **Change the keys:** in the game, Options, Controls. The horn stays on N (the game has no horn of its own).
- **Drift mode (X):** in free roam, the car handles as in the game's drift races (its drift tuning and the drift
  assist), the camera is the drift races' (C still goes through all of them), and drifts score as there: the drift
  races' panel (Best, the multiplier with the points its next level needs, Total) shows under Bank, and DRIFT counts
  a drift's points up while you drift, as in the races: lime when it banks, red when a wall loses it. Free roam's
  music plays on (the drift races have none). Just for fun: the points never go into the bank or the career. X again
  turns it off and gives your camera back. It is off in races and outruns and comes back in free roam after them.
- **Photo mode (F12, or Photo Mode in the pause menu):** while you drive (a race, free roam, an outrun) the world
  stands still, the HUD goes and the camera comes loose from the car. Orbit (the first) turns around a point at your
  car, which you can move up to 10 m from it; Free flies up to 20 m from the car. Drag with the mouse (or the arrow
  keys) to turn, W A S D to move, Q and E for down and up, the wheel to come closer or go farther, Z and X to zoom,
  C and V to tilt, Shift to go faster, Tab for orbit or free, R to start over, H to hide the panel. F12, Enter or
  Space takes the photo, Esc leaves. On a controller: the right stick turns, the left stick moves, LB and RB are down
  and up, the triggers closer and farther, the D-pad zooms and tilts, Y is orbit or free, X starts over, A takes the
  photo, B leaves, View hides the panel. On a touch screen: drag to turn, two fingers to move, pinch for closer or
  farther, the panel's buttons for the rest. The sound effects pause, the music plays on. In free roam with other
  players your world stands still for you only: they see you as paused and drive through you. Not in races over the
  network (Online, LAN, free roam's races with other players). Nothing keeps the camera out of walls.
- **Mouse:** works in the menus; in the garage, drag with the middle button (the wheel) held to turn the car, as in
  the PC game. On the ECU and Turbo tuning graphs the wheel moves the dot of the column under the pointer, as in the
  PC game, and so does a drag up or down on that column. **Touch:** a tap is a click there; drag two fingers to turn
  the car; drag one finger up or down on a tuning graph's column.
- **Controller:** an Xbox or PlayStation pad works through the browser. The browser shows a pad to the page only
  after a button is pressed on it. The triggers are throttle and brake, the left stick steers, LB is nitrous, RB the
  handbrake, A and X shift, Y and B are camera and look back, View respawns, Start pauses; D-pad up starts an event
  or takes an outrun, left and right are the map and messages. A click of the left stick is the horn, unless your
  car has hydraulics: then the stick clicks are the game's hydraulics (left: pressurize, right: bounce). In free roam
  with other players the right stick is quick chat: a flick for a group, another for its phrase.
- **Steering wheel:** a Logitech G29, G920 or G923, and other wheels the browser shows as a game controller. The
  wheel steers, the pedals are throttle and brake (and reverse), the paddles shift down and up, Cross (A) is nitrous,
  Square (X) the handbrake, Circle and Triangle (B and Y) look back and camera, Share respawns, Options pauses, PS
  opens the page menu; the red dial and Enter work the menus. The game gets a wheel, as the PC game did with one:
  the car's wheels follow yours, with no delay. Full lock is a quarter turn of a 900° wheel (270° from lock to lock).
  No browser maps a wheel, so the page takes the layout of a Logitech G29, G920, G923, G25, G27 or Driving Force GT
  from what is known of that model (the G29's gas, brake and clutch are its Z, Rz and Y axes, the G920's Y, Z and
  Rz), as Chrome, Firefox and Safari each place those axes, and guesses any other wheel's from how it reports
  itself. If steering or a pedal is wrong, or you want another full lock, open Controls, Controller and press Set up
  the wheel: turn it to full lock, then press each pedal. That is kept in this browser. The clutch does nothing (the
  game has none). A browser cannot drive a wheel's motor: no force feedback.
- **Shifter:** the game's gearbox shifts one gear at a time, so a shifter (the Driving Force Shifter on a G29, G920
  or G923 for PlayStation, the G25's and G27's own) works as a sequential one: each move of the lever into 1st, 3rd
  or 5th shifts up, into 2nd, 4th or 6th down, and the lever goes back through neutral for the next shift; in reverse
  it holds the handbrake. The paddles still shift. Controls, Controller, Shifter can turn it round (pull to shift
  up), make every gear the handbrake, or turn it off; that is kept in this browser per wheel model. The Driving Force
  GT's own shifter shifts up when pulled back, down when pushed.
- **The page menu on a controller:** Home, or both sticks pressed in, opens it (where the browser passes Home on; on
  Windows the Game Bar may take it). The D-pad or left stick moves in it, A picks, B closes it; meanwhile the game
  gets no controller input. A stick pressed in alone reaches the game a moment later (0.12 s), so the shortcut never
  honks or works the hydraulics.
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
| Online | its own section under Saves: how many play on this server now, free roam with others, your name (in free roam and online), crews (with their colours), your horn, room, ghost mode, the game's Online mode (on by default); more: the free-roam network |
| Game | language, voices, movies, subtitles, autosave; more: startup movies (skippable, original or off), sound, pause or keep running in the background, sound output latency |
| Display | full screen on Play (off: the game plays in the browser window; on by default on phones), resolution scale (phones start at 75%, 100% once one shows it has the room), frame rate (auto follows the screen: 120 on a 120 Hz display, 240 on a 240 Hz one; 30, 60, 120 or 240, where 30 keeps a phone that cannot hold 60 steady; Online and LAN races at most 120); more: menus and HUD (original or upscaled), HUD layout, movie framing, cutscene bars, other cars' headlights (players, all cars, off), frame-rate counter |
| Controller | rumble; more: controller on or off, menu buttons, dead zones |

- **Language:** English, French, German, Italian, Spanish, Dutch, Swedish, Danish, Korean, Chinese, Japanese, Russian,
  Thai, Portuguese (Brazil), Polish or Turkish, for the menus and all the game's text. Where the server has the [language packs](VPS.md#languages), French, German, Italian, Spanish, Japanese and Russian also play with their dubbed voices and movies,
  Chinese, Japanese and Thai with their full text, and Portuguese, Polish and Turkish with their text at all (without
  the packs they play in English, with English voices). Your browser fetches only your language's files: the dubbed
  voices are about 50 MB (Russian 94 MB), and the dubbed movies are smaller than the English ones they replace.
  The start page follows the same choice (the list at its top sets it too): its text, settings and messages in
  that language, English until you pick another. Its problems and errors carry a short code (`GPU-ADAPTER-WIN`),
  the same in every language, to quote when you ask for help; the log stays English.
- **Voices and movies:** Dubbed (the default) plays with the language pack's dubbed voices and movies in languages
  that have a dub (French, German, Italian, Spanish, Japanese, Russian, and Portuguese with ZANIT's dubbing). Original
  plays the base English voices and movies with the chosen language's text.
- **Subtitles:** on by default: the movies' lines as text (the PSA, the tutorials and the story) in every language
  but French, German, Italian and Spanish, whose movies are dubbed. English's are VileStuff's
  [English Subtitles Fixed](https://nfsmods.xyz/mod/5286), with who says each line, where the server has its
  [language pack](VPS.md#languages) (without it English has none: the game's own English lines stop at their first
  apostrophe).
- **Menus and HUD:** Original (the default) is the game's own. Upscaled shows LunicAura106's
  [Upscaled HUD](https://nfsmods.xyz/mod/4319) where the server has its [texture pack](VPS.md#texture-packs): the
  menus, the race HUD, the fonts and the car and brand logos redrawn at four times their size, sharp on a large
  screen. The game's layout stays as it is. Your browser fetches the pack only with Upscaled (about 5 MB to
  download). A language with fonts of its own (Russian, Polish, Chinese, Japanese, Thai) keeps them. Most phones
  decode the larger textures into four times the memory.
- **Radio:** the menu (F9) has a Radio line. It plays the music files of a folder on your device in place of the
  game's EA Trax songs. The folder icon picks the folder, its subfolders included; the list icon opens the Playlist:
  each track is Menu, Race, All or Off. A new song starts with the next track that suits the place, and the game's
  plaque shows its artist, title and album (the tags of an mp3, else the file name, else the subfolder's name). A
  file that cannot be read is skipped. The files stay on your device. The folder and the switch are remembered; after
  a reload the browser may ask for access to the folder again, then click the Radio line. Firefox and Safari pick the
  folder too, but cannot remember it: pick it again after a reload. Radio off leaves the game's EA Trax as it is.
- **More settings:** each group's "More … settings" has the less common ones, and says how many of them you
  changed. **Reset** next to a changed setting puts its default back.
- **Edit the file:** "Edit settings.ini as text" opens the file itself. A line the game would ignore is shown with
  the reason. A line that sets a runtime variable by its own name (`NFSU2_ROAM = 1`) shows on the row it belongs to,
  and every such line is listed under "Other variables in this file". Lines that would name a file of your saves or
  drive the game by itself (`NFSU2_REGFILE`, `NFSU2_INPUT_SCRIPT`, ...) are ignored: only the page sets those.
- **Links:** settings in a page address (`?env=...`) work only on a development server (one started with `--dev`).
  On yours a link cannot change your game; the start page says so when an address carries them.
- **The look:** the game looks as it did originally. Any graphics enhancement will be a separate setting, off
  unless you turn it on. One fix is always on: a car's shadow lies 1.5 cm above the road instead of 4, so its edge
  no longer cuts into the bottom of the tyres (TerminatorVasya's [Shadow Fix](https://nfsmods.xyz/mod/7328)).
- **The European cars:** the Peugeot 106 and the Vauxhall Corsa 1.8, which only the European release sold, are in the
  car lot and on every car select (Quick Race, Customize, LAN and Online), and AI racers drive them too. The North
  American game has their files but hid them.
- **Power Units:** the game's Options, Gameplay (also in the pause menu's) has a row after Speedometer Units: the
  engine's power in bhp or kW wherever the game shows it (the dyno, the performance shop's parts), whatever the
  speedometer shows. Until you change it, it follows Speedometer Units, as in the original game (kW with km/h).
  Torque still follows the speedometer (Nm with km/h, ft-lbs with mph), and the dyno results' 0-60 and 0-100 are
  mph times either way.
- **Two tabs:** one browser runs one game at a time. A second tab shows "Open elsewhere" and its Settings are
  read only. It takes over when the first tab closes.

## 5. Saves

- **Where they are:** your profiles and career are saved **in this browser, on this PC** (the site storage for
  the server's address). They are not on the server, unless you use cloud saves (below). Each address keeps its own
  save store, so stick to one.
- **Cloud saves:** Enable cloud saving (in Saves, beside Cloud saves) keeps your saves on the server too, under a
  keyphrase only you have: `nfsu2-xyz-` and 8 words. In another browser (another PC, a phone), enter it in the box
  and press Load: any case, with or without `nfsu2-xyz`. There is no account and no email.
  - The page locks the saves with the keyphrase before they leave the browser: the server keeps bytes it cannot
    read, under a code made from the keyphrase. Keep the keyphrase safe, in a password manager say: it is the only
    way back to these saves, and nobody can look it up for you. The box shows its first 3 characters; the eye shows
    all of it and copies it, the arrow downloads it as a file.
  - From then on that browser syncs by itself: its saves go to the server after the game saves and when it closes,
    and a newer save from your other browser comes in when the page opens and when you press Play. If both browsers
    changed them since they last synced, the page asks which to keep: it shows both, their profiles, money and when
    they were saved, marks the newer one, and asks again before either replaces the other. Nothing syncs until you
    pick, so Play asks first. A send the server kept without the page hearing back (a timeout, a frozen tab) is no
    conflict: the page knows it as its own. Before the cloud's saves replace the ones in this browser (Use the cloud's,
    Load, Go back), the page keeps a copy of yours here for 30 days, and Put back brings it back (swapping again is
    possible). A browser with no profiles never sends its
    empty saves by itself: it offers the cloud's instead (keeping the empty ones asks first: every browser on the
    keyphrase loses its profiles). The page also asks before cloud saves with no profiles, or older than the ones in
    this browser (a server gone back), replace yours.
  - The profiles and `settings.ini` go. What belongs to the device stays in each browser: F12 screenshots, DVD and
    magazine covers (a cover is a picture of 1 MB: another browser's crib lists it, but the game says it cannot load
    it), the game's graphics options, and in Settings full screen, the resolution scale, render size and pixel limit,
    Menus and HUD, the frame rate and its counter, the output latency, the controller's settings and how many
    players' cars free roam shows. A phone keeps its own and a PC its own.
  - The server keeps the copy of an earlier day too: "Go back to ..." brings it back when a day went wrong.
  - Stop syncing leaves the saves where they are, and the keyphrase in the box; Delete from cloud deletes them on the
    server (your browsers keep theirs). A keyphrase nobody used for two years is deleted from the server. A browser
    whose keyphrase the server no longer has keeps it and sends nothing; Sync now looks again and offers to send its
    saves there.
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
- **Screenshots:** F12 while playing saves the picture as a PNG beside your saves. Saves on the start page shows
  them, newest first; click one to see it large, page through with the arrow keys, download or delete it.
- **Backup:** Saves, Download a backup gives one zip with your profiles, the game's options (`registry.txt`),
  `settings.ini` and F12 screenshots.
- **Restore:** Saves, Restore a backup puts a zip back, on this PC or on another one. It replaces what this browser
  has, after showing what is in the zip, including what its `settings.ini` would change on the network (free roam
  on, the name others see). A very large backup keeps every save and settings file. Only screenshots past 512 MB
  are left out. Restore only backups you made yourself or trust.
- **Profiles from the PC game:** Restore a backup takes them too. Zip the folder `%LOCALAPPDATA%\NFS Underground 2`
  (type that in Explorer's address bar), or pick one profile's own file in it (`<NAME>\<NAME>`, no extension). They
  are added beside the profiles in this browser, with their DVD and magazine covers; one of the same name is
  replaced, after asking. A profile saved by another version is left out, as the game would not load it. One
  changed by a save editor is restored with a warning: a value the game would not make itself may crash it or break
  the career. One with cars a mod added is restored with a warning too: this game does not have those cars, and may
  crash or show them wrong.
- **Backups are your safety net.** When the disk is nearly full, the browser may clear a site's storage unless it
  promised to keep it. Chrome decides that by itself, and the page cannot see how full the disk is, so it does not warn.
  Firefox asks by the address bar (the Saves section says so while it asks): allow it. Make a backup now and then,
  or use cloud saves.
- **Preloading the game data:** "Preload the game data" in Game data on this PC fetches the game folder but the
  movies into this browser, compressed on the way, with its progress and the time left. You can play meanwhile:
  preloading pauses while the game runs and goes on when it ends. Stop it whenever you like; what came is kept, and
  "Preload the rest" goes on from there, also after a reload. It needs about 2 GB of free disk space. The page says
  so before it starts when the browser has less room.
- **Game data on this PC:** "Delete the game data on this PC" frees disk space on this PC. Your saves stay; the game
  loads the data from the server again when it needs it.

## 6. Free roam with other players

Free roam is shared with **everyone** on the server who turns it on. Join a Crew to find your friends among them,
or use a Room to keep to a group.

1. Each player turns on **Online, Free roam with others** on the start page. The first Play asks too (Turn on or
   Not now, and Esc is Not now); it asks once, and the switch changes the answer later. While the server's free
   roam is busy (it gives each player fewer than 24 cars) it doesn't ask, and asks at a later Play instead.
   **Online, Your name** is the name the others see (empty: your profile name); the question shows it once there
   is one, to change.
2. Everyone plays from the same server's address.
3. Each player starts or loads a career and drives in free roam. Players find each other by themselves, and their
   cars appear in your city with their names over them, as in the game's own LAN races.

Good to know:

- **How many:** up to 6000 players on a server set up as VPS.md describes (2000 with the older Node server), 128 on
  one started by hand (its owner can change that). Only free roam and the LAN and Online screens take a place;
  single-player takes none. When it is full, you wait in line for a place, first come first served: the top of the
  screen shows your place and, once a few went in, about how long it takes. You play alone meanwhile and join the
  others by yourself when it is your turn. The page menu's "Stop waiting: play offline" leaves the line for the rest
  of that game, and when the server is full as you press Play the start page asks whether to wait in line or play
  offline (or, the first time, says so in its free-roam question). The server pairs each player with the 32 players
  nearest to them, and your game shows all 32 of their cars at once. Their names show over the cars within 100 m, and
  each gets an arrow on the minimap when it is near and a marker on the world map (M), as the game's own opponents
  do; the world map shows where they were when you opened it. A player in a race, a shop or the menus is not in your
  free roam: no car, no arrow, no marker until they are back. A player farther than 300 m sends
  you their car 5 times a second instead of 20 (past 1 km, twice), which saves traffic and doesn't show at that
  distance. When many players arrive at once, their cars load one after another, about two a second.
- **Ghost mode:** off by default: you collide with the other players' cars (Online, Ghost mode; on: you drive
  through them, and they through yours). The page menu (F9) switches it while you play,
  at once, and keeps it for your next games. A car that appears on top of yours, at a garage or
  after a jump, lets you through until you are apart. Each player's own game moves their car: a car you hit is pushed
  in its owner's game, and moves on your screen as it does there. A hit is shared between the two cars by their
  models' weight (0.96 t for the Peugeot 106 to 2.9 t for the Hummer): driving into a standing car of your weight,
  you go on at about half your speed and it leaves at about the other half, where you used to stop dead and it flew
  off faster than you came; driving a 240SX into a standing Hummer, you keep about a quarter of your speed and it
  leaves at about a third of it. Such a hit starts the game's slow-motion crash view only when it is hard: from about
  155 km/h of closing speed between two cars of the same weight. You also drive through a car whose player is
  paused (or you are), one that has stood still for a minute (or yours has), and any car within 20 m of a shop's, the garage's or the car lot's door, so a
  crowd can't wall off an entrance. A car that has stayed within 6 m of one spot for 20 s, after at least four hits
  from at least two other players' cars in the last minute, is in the way: everyone drives through it until it
  leaves that spot, so nobody can park across a road (the airport's way out) for long. The same goes for the start of an event
  that is open in your career (a race, or one of the stage's other events): within 20 m of it you drive through every
  car, and every car through yours, so nobody blocks it; for players who don't have that event it is an ordinary
  spot. A player on an older version doesn't know about your events or your Ghost mode setting, and collides with
  you there as before.
- **Keep right:** a new career starts in the airport's car park, and every new player drives out of it into the
  others. As your car leaves that spot, the top of the screen says "Please keep right to help avoid crashes" for 15 s
  (while other players are in free roam).
- **Names over the cars** follow the game's own player-names option, as in its LAN races. A player who paused shows
  as "paused" there.
- **Horn:** hold N (or click the controller's left stick) to honk. The others hear it from your car: louder the nearer
  they are, from the side your car is on, and lower in pitch as you drive away from them. Pick yours in Settings,
  Online, Your horn (the play button next to it lets you hear each one): dual tone, single tone, air horn, train horn,
  Klaxon, La Cucaracha (it plays while you hold it), clown horn or sad trombone. The others hear the one you picked;
  players on an older version hear the dual tone.
  Each player's horn is also pitched a little differently by their name, so two players with the same horn sound
  apart. It sounds while the game runs, not in the pause menu or on the map. It works in Online and LAN races too.
  How loud the horns are (yours and the others'): the game's Options, Audio, Horn Volume, also in the pause menu's
  Options, from 10 % to 100 % (the most, and the default). Moving it sounds your horn at the new volume.
- **Quick chat:** say one of your sixteen set phrases to the players near you. There is no typing, so there is nothing
  to read that someone made up, and everyone reads a phrase in their own language. Press 1 to 4 for a group, then 1 to
  4 for its phrase; on a controller, flick the right stick (up, right, down, left) for a group, then again for the
  phrase; on a touch screen, the chat button shows all sixteen. Esc closes it. These are the sixteen you start with:

  | 1 Hi | 2 Reply | 3 Drive | 4 Race |
  |---|---|---|---|
  | Hey! | Yes | Follow me! | Race? |
  | Nice car! | No | Wait up! | Join my race! |
  | Thanks! | Sorry! | Let's go! | gg |
  | Bye! | lol | Nice! | One more? |

  Settings, Online, Your phrases, Choose phrases puts any of about 150 phrases in each place (greetings, replies,
  thanks and sorry, compliments, driving together, car meets, races, reactions, help) and names each group.

  A phrase shows over your car and in the chat log on the left, for the players within 250 m (where they hear your
  horn) and everyone in your race lobby, wherever they are; in the lobby's card it shows beside your name. Over a car
  only while its name shows (within 100 m, in front of you, the game's player-names option on). Three phrases in six
  seconds at most; then the picker asks you to wait. Settings, Online, Quick chat turns it off: you then neither see
  nor send phrases, and the others' games send you none. Players on an older version neither see yours nor send any,
  and those who have not reloaded the page since the phrases came see only the first sixteen.
- **Engines:** the others hear your engine from your car, as you hear it yourself: the game's own recordings of your
  car's engine (each car has its own, and the engine upgrades change it), pulling as you accelerate and letting off as
  you lift, at your revs, with the limiter and a launch's wheelspin as your game plays them; louder the nearer they
  are, from the side your car is on, duller far away and lower as you drive away. Every player's car sounds, however
  many there are: the game itself gives an engine only to a few cars, so before, the first three players near you
  sounded like one of its AI racers and the others not at all. A player on an older version sends no engine: their
  car sounds from its revs and throttle, as well as those tell; their game still has the old engine sounds.
- **Hydraulics and nitrous:** the others see your car lift, tilt and bounce on its hydraulics, with its wheels on the
  road, and your nitrous as the game draws it on any car: the light trails behind it. They hear them from your car
  with the game's own sounds, as you hear your own: the nitrous going on and off, letting it out while standing or
  off the throttle, each wheel's pump and valve and a landing, louder the nearer they are. A player on an older
  version sees the body move but not the wheels, and no nitrous, and hears none of it.
- **Tyre smoke:** when you slide, spin your wheels or do a burnout, the others see the smoke from your tyres, as the
  game draws it for any car. Between players on different versions, neither sees the other's.
- **Headlights:** the other players' headlights light the road in front of their cars, as yours do, a little dimmer
  and in their own colour; in Online and LAN races too. Display, more: Other cars' headlights (All cars: AI racers and
  traffic too; Off: only yours, as in the original game). The Windows plugin draws only your own.
- **Outruns:** stay within about 25 m of another player for 4 seconds, away from the shops' and the garage's doors
  and from either player's open events (20 m), and the game offers an outrun, as it does with
  its own tuners: the outrun icon lights up and the top of the screen says "Press Enter to challenge ... to an
  outrun" (on a controller, D-pad up, the game's Engage). The other player sees "... challenges you to an outrun" and
  takes it with Enter as well, or lets it go: a challenge lapses after 15 seconds, or when you are more than 60 m
  apart. Then both games run the game's own outrun against each other's car: its "Feet" meter, and whoever gets
  1,000 feet (300 m) ahead of the other wins, and the bank pays or charges as in an outrun against a tuner. Leaving it
  (back to the menus, quitting, losing the connection) loses it. The other players' cars stay, and they see the two
  drive. Players on the Windows plugin are not offered outruns.
- **Races with other players:** a race of your career can be raced with the players around you, as the game's own LAN
  race on its track, in your career car. That takes a car of your own: in a new career's prologue (the 350Z you
  start in) you cannot join or open a lobby, and the panel says so; from your first car on you can.
  - **An open race** (its marker in the street): its card asks Accept or Decline as always, and Accept asks once more:
    **Solo** (the career race, as before) or **Multiplayer**.
  - **A race that is not open** in your career has no marker of the game's. Within about 50 m its start shows
    "Circuit (multiplayer)" or the like, and at the start a panel on the right (under Bank) says it is multiplayer
    only there and why (you won it in your career, your sponsor's contract has no place for it, a URL race before
    its wins, an SUV race without an SUV, not open yet), and that Enter opens a lobby. A spot offers the race of your
    stage there, else the last one you won there: never one of a stage you have not reached. None in the prologue,
    none while a story task runs, and none where the game has something of its own to enter at the same place (a race
    open in your career there, a cover shoot): Enter is the game's there, and an open race's card asks Solo or
    Multiplayer as above.
  - **Multiplayer opens a lobby** at the start: the panel shows it, its race, career stage, track and laps and its six places (drag
    4, 3 on the three-lane track) with the players in them, the host first and yours marked. The players near see
    "Thom's circuit (1/6)" over the start (from about 250 m); everyone in free roam sees its race's icon on the world
    map (M), and the nearest four lobbies their words beside it ("Circuit 1/6"). They join at the start: within 15 m
    of it the panel shows the lobby and Enter joins (at an open race: its card's Accept, then **Join**). Up to 6
    players; at a full one the panel says so, and Enter starts another. Drive more than 40 m from the start to leave
    a lobby; the host leaving calls it off, and a lobby nobody joins closes after 5 minutes. A host who seems away
    (none of the game's keys or controller buttons and no driving for 2 minutes while others wait) gets a 30-second
    countdown on every panel of the lobby, then it closes; one of those keys keeps it. A lobby is within your Room, as
    everything in free roam. At a race's
    start Enter is for the race: no outrun is offered there.
  - **The host presses Enter** to start once another player is in. After a 5-second countdown every game in the lobby
    goes to the race by itself, behind a screen with the lobby on it and where the trip is ("Loading the race"): out
    of free roam and through the LAN menus (your keys and controller do nothing meanwhile) to the race on the career
    race's track, direction and laps (a URL event: its first race; a LAN race takes at least 2 laps, Street X 4),
    with no traffic and no AI racers. You drive the race. After the results every game goes back by itself,
    behind the same screen, through the main menu and your crib, to where it left free roam, and the panel says your
    place and prize: about a minute for the way out and back.
  - **The cars are evened out** in circuit, sprint, drag and URL races: at the start every game rates the race's
    cars as the game itself does (its performance rating: the engine, gears, weight and drag with every part in) and
    changes each car's engine power so the cars come close: they head for a rating between their middle and the
    best (two cars: two thirds of the way up from the slower one), and each goes nine tenths of its way there, so
    the better car keeps a small edge (of two cars, one rated a third higher, about 3 % is left). In a drag race,
    where even that edge decided most races, each goes all of its way there. The power changes
    by at most half again as much or a quarter less (the nitrous with it). No car keeps a stock car's top-speed
    governor (as with an ECU part), so the extra power counts at speed too. Grip, brakes, gears, weight and your
    tuning stay as they are, so your car still drives as itself and your upgrades still count, by less; a car far
    ahead of the others stays ahead, by less. The lobby says so before you join, and after the race the note says
    how your engine was changed. Street X and drift races leave the cars as they are: there the rating did not decide who
    won.
  - **Your career is not touched:** the race does not count in it, is neither won nor lost there and opens nothing.
    Only your bank changes: 1st $400, 2nd $300, 3rd $200, 4th $100, nothing for 5th or 6th. **Quitting the race**
    (its pause menu's Quit) costs $75, as the panel says before you join: the host's quit ends the race for everyone,
    and in a race of two either player's does. A race that ends under you costs nothing. It goes in or out at the crib
    on the way back, and the crib's autosave saves it with your profile (with the game's Save option at Prompt or Off
    it waits for your next save, as any change in the crib does).
  - A player who leaves the lobby in the countdown, quits the game, or whose game cannot set off is not waited for; the
    host's game waits a minute at most for the others in the LAN race's room, then starts with those who are ready.
    A race that cannot come together (the host's game gone, nobody else in the game in time) ends the trip: every game
    goes back to free roam by itself and says why. Players on an older version or the Windows plugin see no lobbies.
- **Crews:** friends who type the same crew name (Settings, Online, Crews, or the page menu (F9) while you play) always
  see each other, however far apart and however busy the server: the server pairs crew-mates first, before the
  nearest players (up to 16 crew-mates each), and your game gives their cars a place first. Their names show over
  their cars at any distance, with the distance past 100 m ("Sam (1.2 km)"), in front of other names, and their
  minimap arrows and world-map markers (with their names beside them) are in the crew's colour. You can be in up to 4
  crews; each has its own colour (cyan, magenta, violet, red unless you tap its dot to pick another), and the crew
  list shows how many of each are in free roam now (in the page menu, you count too once you drive). A crew of more
  than 64 players in one room is too big to pair first: they see each other only nearby, and the list says so (pick a
  less common name). Capitals and spaces don't count ("Blue Room" is "blueroom"); a name that reads as a slur is no
  crew. Everyone else is still there, unlike a Room. Not private: anyone who types the same name joins, but other
  players never learn your crews: the server tells each game which players near it share one. A player on an older
  version is in no crew.
- **Rooms:** set the same "Room" to see only some of the players' cars. A room is a filter between players, not
  privacy: every browser on the server still sees your name ("Your name", or the profile name), whether you are in
  the menus or driving, and the room, and someone with their own tools could join it. Play on a server only with
  people you would show your name and car to.
- **Who sees what:** with free roam off (the default), the game does not join the server's network at all. With it
  on, everyone on the server sees your name and status, and players in free roam with you see your car and where it
  is. The server reads where your car is, to pick the players near you, and keeps nothing of it. After a multiplayer
  race your game tells the server your place and your car as the game rates it (its type, power, weight, top
  speed), and its log keeps that with your name, for evening out the cars. Nobody sees your real address: the server
  gives each player a virtual one (`10.77.x.x`).
- **A tab in the background** (or another window in front) pauses your game. The others see you as paused: your car stays where you stopped, with "paused" after your name, until you come back.
- **Closing the tab:** the others see your car disappear after a few seconds.
- **Messages over the game** say when a player joins free roam, goes into a race, is back in free roam after it,
  starts a multiplayer race nearby, or leaves. The menus and the shops are not announced.
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
  (`Hakan1995` and `Dani99` are names). The same goes for Nazi names (and `1488` in a name), hate aimed at a group,
  terror and sexual violence. Swear words are fine.
- **Your name is yours while you are online**, and while you race. Someone else can use it when you are not; the
  rankings and your EA Messenger lists follow the name, so whoever plays under it while you are away sees your
  friends, the players you block, your friend requests and the messages waiting for you, and can change them. If
  another player is online under it, you play under it with four digits in place
  of its end (`NAME` becomes `NAME4821`), usually the same digits when you come back. Set **Online, Your name** to
  keep your own.
- **No name and no profile:** the game's own EA Login screens come instead. **Use Existing EA Account**, any name
  (3 to 15 letters, digits, `-` or `_`) and any password, **Continue**; **No** to "create a profile"; your persona.
  Nothing is checked and nothing is kept: there are no accounts. Type a password you use nowhere else: the game
  sends it to the server only lightly scrambled (the server never reads it).
- **Kept on the server:** only race results per name (Rankings) and your EA Messenger lists: your friends and the
  players you block (names), the friend requests you sent until they are answered (30 days at most), and your friends'
  messages that came while you were away until you are back (7 days at most). Never your password: the server does
  not read it.
- **Game passwords:** the game asks for the password of a game created with one, and only the same password joins
  it. The server keeps no copy of it, only a check value for that game while it is open, and never shows or logs it.
- **Others see** your name, your game, your chat and your results, never your real address.
- **Every track is open** when you create a game, either direction, whatever your career has unlocked, and a new
  game starts at **2 laps** (the game's own default was 3; Options, Laps changes it). A drift game takes **2 laps** or
  more, as a circuit, ranked too (the game's own least was 4). The same goes for the game's LAN races. The career and
  Quick Race keep their unlocks.
- **Up to 6 players** race in one game (Options, Max Players; the game's own most was 4, and 6 only in URL races).
  Drag keeps one car per lane: 4, or 3 on the track with three lanes.
- **Horn:** N (or the controller's left stick click) honks in a race too, as in free roam: the others hear the horn you
  picked from your car. The same in the game's LAN races.
- **The Lobby lists every open game**, whatever its race type and options (Collision Detection, Race Breaker, Car
  Performance Restrictions, Ranked), those its **Filter** asks for first, until you set the Filter: after Apply
  Filter (or Back) on its screen it lists only the games the Filter asks for. Set the Filter back to its defaults
  (Circuit, no restriction, both options on, Unranked) to see every game again. A game you create or join gives the
  Filter its own settings (the original game does that), and the list is the whole one again until you next set it.
  In the original game the Filter always hid all others, so a game made with Collision Detection off was seen by no
  one. A game with **Private Room** on is not listed, as in the original game.
- **Ranked games** need three players at least. Ranked wins give ranking points. After a ranked race the results
  screen lists whose results came in (received or waiting), then, once the games of two players who finished sent
  the same result, Official Race Results: each player's place, time (drift: score) and the ranking points the race gave (100 for the
  winner down to 0 for the last; 0 for a player who left the race, shown as Disconnected).
- **A race counts** in Rankings once the games of two players who finished it sent the same result (every game
  sends its own at the results screen), so a race that only one player finished does not count. A player who left
  the race decides nothing: their game counts every player as gone. Each player races from their own browser.
- **EA Messenger** (the Online Main Menu's button): Search finds the players online on this server and those with a
  race in Rankings. **Send Friend Request** reaches the other player now, or when they next come online: they see you
  in their list with a green arrow and pick **Answer Friend Request**: Accept, Decline or Block. Your friends stay on
  your list and are told when you come online; **Remove Friend** takes the two of you off each other's lists. A
  message to a friend who is away waits for them (7 days), and shows with an envelope when they are back; a message
  to anyone else reaches them only while they are online. Players you block stay blocked: nothing of theirs reaches
  you.
- **Who is on:** Online on the start page shows how many play on this server now, and how many of them are in Online
  mode (in its menus or racing). Only the two numbers: no names.
- Turning it off: Online, Online mode on the start page (the game then cannot connect).

## 8. When something is wrong

| What you see | What to do |
|---|---|
| "Secure, isolated page" on another PC | Open the server's `https://` address, not `http`. Only `localhost` works without HTTPS. |
| "Graphics (WebGPU)", `GPU-OFF` and the other `GPU-OFF-…` | The browser has no WebGPU, which the game needs. Chrome: update it (on Linux also turn on `chrome://flags/#enable-unsafe-webgpu`). Firefox has it on Windows and on Macs with Apple chips; elsewhere (Linux, Macs with Intel chips) play in Chrome, or set `dom.webgpu.enabled` to true in about:config. Firefox for Android has none yet: play in Chrome. iPhone and iPad: WebGPU comes with iOS 26 and iPadOS 26. `GPU-NOT-SECURE`: see "Secure, isolated page" above. |
| `GPU-ACCEL-OFF` | Chrome uses no graphics at all (it draws WebGL in software, or not at all): "Use graphics acceleration when available" in `chrome://settings/system` is off, or Chrome gave up on the graphics after they crashed. Turn it on and restart Chrome; if it was on, update the graphics driver. |
| `GPU-ADAPTER-LINUX`: Chrome on Linux, while `chrome://gpu` says WebGPU is hardware accelerated | Most likely the Vulkan driver is missing: Chrome runs WebGPU on Vulkan there (the rest of Chrome on OpenGL). `ls /usr/share/vulkan/icd.d/` lists the installed ones (`intel_icd` for Intel, `radeon_icd` for AMD). Install it with `sudo apt install mesa-vulkan-drivers` on Debian and Ubuntu, `sudo dnf install mesa-vulkan-drivers` on Fedora, `vulkan-intel` or `vulkan-radeon` on Arch, then quit Chrome completely and start it again. Chrome has WebGPU on by itself on Linux for Intel graphics (Gen12 and newer) and NVIDIA (Wayland); for other graphics turn on `chrome://flags/#enable-unsafe-webgpu`. If the driver is there, the Dawn Info section further down `chrome://gpu` lists each adapter WebGPU sees and why one is blocked. |
| `GPU-ADAPTER-WIN`: Chrome on Windows | Chrome runs WebGPU on Direct3D 12 there, and older graphics have no Direct3D 12 driver (Intel HD 4000 and older, for one): the game cannot run on that computer. The Dawn Info section further down `chrome://gpu` lists each adapter WebGPU sees and why it is blocked; when it lists only "D3D11 backend" adapters, "Blocklisted - D3D11 backend not fully implemented", the graphics have no Direct3D 12. On newer graphics, update the graphics driver from Intel, AMD or NVIDIA, then quit Chrome completely and start it again. |
| `GPU-ADAPTER-WIN-ARM`: Chrome on Windows on ARM (Snapdragon) | Chrome keeps WebGPU behind a flag there: turn on `chrome://flags/#enable-unsafe-webgpu`, then restart Chrome. |
| `GPU-ADAPTER-ANDROID` | Chrome on Android has WebGPU from Android 12 on Qualcomm (Adreno), ARM (Mali) and Intel graphics, and from Android 16 on Imagination (PowerVR). Update Android and Chrome. If that does not help, turn on `chrome://flags/#enable-unsafe-webgpu` (on the phone, type it into Chrome's address bar, or open `chrome://flags` and search for "Unsafe WebGPU"), then restart Chrome: that lifts Chrome's WebGPU blocklist. Samsung's Xclipse (some Galaxy phones) is on it, and the game runs with the flag (a player's Xclipse 540 with Android 16). If the game still does not run with it, it cannot run on that phone yet. |
| `GPU-ADAPTER`, `GPU-ADAPTER-FIREFOX` | The browser would not use the graphics. Restart it; update the graphics driver (on a Mac: macOS) and the browser. `chrome://gpu` in Chrome, about:support (Graphics) in Firefox says why. In Firefox on Windows, graphics without Direct3D 12 cannot run it either. |
| `GPU-ADAPTER-AFTER-LOST` | The graphics stopped during the last game (`GPU-LOST`), and the browser has not given the reloaded page graphics since: not graphics too old. Chrome keeps a site whose graphics hung off them for a while (two minutes); the page asks again every 10 seconds, and Play comes on once they are back. If it stays off, quit the browser completely (every window) and start it again. If the graphics stop again, update the graphics driver; two players' Intel Iris Xe hung this way (`DXGI_ERROR_DEVICE_HUNG` in the log). |
| `GPU-NO-BC`: a note about packed textures | Nothing to do: the game plays. This GPU (in many phones, Pixels among them) cannot read the game's compressed textures, so the page unpacks them as the game loads: about 200 MB more memory, and loading screens may pause for a moment. Graphics that can read ASTC instead (iPhones, iPads, most other phones) get no note: most textures are converted to that, about 50 MB more. |
| `GPU-INTEGRATED-FIREFOX`: "Graphics: Intel (integrated)" (or AMD) in Firefox on Windows | Firefox runs the game on the graphics it draws its own windows with, whatever the game asks for. If the computer also has a graphics card, as many laptops do, give Firefox the card: Windows Settings, System, Display, Graphics, Firefox, High performance; then quit Firefox completely and start it again. On a computer with integrated graphics only, nothing changes. Chrome does the same (its `chrome://gpu` names the graphics it uses): the same Windows setting for Chrome, or `chrome://flags/#force-high-performance-gpu`. |
| `GPU-RENDERER`: "The game was stopped" | The graphics did not start: the game cannot go on without them. Restart the browser and press **Play**; if it happens again, update the graphics driver, and download the log (its `[gpu]` lines say why). |
| `GPU-LOST`: "The game was stopped" | The graphics stopped during the game (the graphics driver reset, or the browser's graphics process crashed): the game cannot go on without them. Press **Play**: the page reloads, which starts the graphics again (Safari gives a page no graphics after that until it reloads), then **Play** starts the game. Your saves and settings stay; the log is cleared by the reload, so download it first if you want to keep it. If the page then says `GPU-ADAPTER-AFTER-LOST`, see that. If it keeps happening, restart the browser and update the graphics driver. |
| "Game module" | The game did not load, or the server is not reachable. The page tries again by itself, or press **Retry**. If the module is missing, start the server with `web/start.sh`, which builds it. |
| "A new version of the game is on the server" | Press **Reload** or **Play**: the page loads the new version, and your saves and settings stay. The log is cleared by the reload, so download it first if you want to keep it. Before any game the page reloads by itself; during a game it says so over the game, and the start page offers it when you quit. |
| "Open elsewhere" | The game runs in another tab of this browser. Switch to it, or close it. |
| No sound | Click the game or press a key. Check the sound setting and the output latency (try "Robust"). |
| Stuck on a loading screen | F9, Quit game; then "Close the game now"; then "Stop the game". When the log ends in `[data]` lines, the game was waiting for its data from the server: it goes on when the data comes. |
| Other players' cars stutter, freeze and jump in an Online or LAN race | All updates go through the player who created the game, so let whoever has the steadiest connection (a cable rather than Wi-Fi or mobile) and the fastest computer create it, and keep the game's tab in front. After the race, F9, Download log: its `[racenet]` lines say how each car's updates came (long waits, how many came at once after them) and each player's frame rate. |
| Anything odd | F9, Download log (or "Download log" on the start page after the game ended) and keep the file. It has the game's own log. When the browser stopped the page itself (on an iPhone the game is suddenly gone and the page loads again), the start page says so and Download log starts with that last game's log. After you reload or close the page during a game, Download log has that game's log as well. It names your browser, its version and your system, your graphics chip, and your time zone's offset from UTC. Names (yours, your profiles', crews' and room's, and those of the players you met) and addresses become stand-ins such as `you`, `player-3k9x` and `vlan-7q2m`, the same in every log from that browser, so you can post it as it is. |
| `Port 8080 is in use` when starting | The server already runs; open its address. Or start on another port: `web/start.sh --http-port 9080`. |
| Full screen | The game plays in the browser window. F9, Full screen goes full screen, and holding Esc leaves it (in Firefox, a press). "Full screen on Play" in Settings starts every game full screen. |
