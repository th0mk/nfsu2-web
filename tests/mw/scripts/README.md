# Most Wanted input scripts

Input scripts for runtime/autotest.c, served at `/tests/scripts/` by a server started with `--game mw --dev`
(see ../README.md).

Besides autotest.c's commands, Most Wanted's runs take `call ADDR [ecx=X] [ARG ...]`, `loadlib NAME`, `objects` (the
kernel objects in use by kind and creator), `movie NAME` (a movie played as the front end plays one), `movieup` and
`movieend` (wait until it is up, then gone) and `waitgone PACKAGE` (runtime/mw/debug_mw.c), all test-only, and the race
aids of runtime/mw/career_mw.c, test-only too, which run at the game's per-frame anchor: `racewin` (a shortcut finish:
the player is finished where it is, with three of the calls the race's script makes at the line), `autopilot on|off`
(the game's AI drives the player's car), `autopilot races` (the same at every race's GO from then on, for races that
follow each other with no menu between, with each race's start and end logged; a lost race is raced again as a player
would: Enter on its results, then Resume Career), `teleport X Y Z DX DY DZ`, `where`, `racestatus`, `frames`, `pursuit
[watch]` / `pursuit wait on|off [N]` (the player's pursuit state: heat, the pursuit's heat range, the pursuit object,
its length, its cops and bust timer; while watching, the game's own end of the pursuit, evaded or busted; the waits are
on the game's state) and `breaker [watch]` / `breaker wait on|off|full|empty [N]`, `breaker wait above MPH [N]`,
`breaker wait charge X [N]`, `breaker wait rise X [N]` (the player's Speedbreaker: on or off, its charge, the car's
speed).

The races: `race_frames.txt` (a Custom Race circuit long enough for it, the anchor counted over 10,000 frames of the
race), `race_loop.txt` (25 Quick Races in a row, each ended by `racewin`, with the kernel objects before each load)
and `race_win.txt` (a Custom Race sprint the AI-driven car wins over the line, saved; seeded with
`profiles_make.txt`'s saves).

The movies: `intro_idle.txt` and `intro_title.txt` (the boot movies and the title, for web/tools/intro-test.mjs
--game mw), `movies.txt` (all 32 movies of the game folder played to their end and skipped),
`media_refshots.txt` (the play page's pictures of the movie screens and the new alias's name entry) and
`credits.txt` (Options > Credits rolled to their end).

The frame rate: `menu_hold.txt` (the main menu's camera swing-in traced, then the menu held for 60 s, a `[status]`
line over it and the clocks in peeks; its check wants the swing over in 6 s of wall time and RealTimeFrames at 60 a
second at the page's frame-rate setting `NFSU2_FPS`, runtime/mw/framerate_mw.c; `NFSU2_HFR_LOG=1` adds a `[mwpace]`
line a second with the slots and the steps the simulation ran). `hfr_race.txt` is the lockstep A/B of a race at
two rates (a circuit with Maximum traffic, the AI driving, 90 s of game time; `NFSMW_HFR_TRACE=1` traces it every 0.25
s of game time, `tools/mw/hfr_ab.py` compares runs; runtime/mw/hfr_mw.c has the fixes it checks).

The race kinds (TRACKER M4-05, M4-06): `race_kind.txt` (a Custom Race circuit, sprint, lap knockout or speedtrap on
one track, picked on the screens by the kind's menu item and the track's name label, `MWK_RUNS` times, each ended by
`racewin`, a shortcut finish before any lap is done: the kind's own rules are not tested there),
`race_kind_end.txt` (the same races driven by the game's AI to their own end, a trace of the racers' progress and
finish on the way), `race_drag.txt` (a Custom Race drag the game's AI drives to its end) and `race_tollbooth.txt`
(the Challenge Series' first event, a Tollbooth Time Trial, ended by `racewin`: Quick Race has no tollbooth; seeded
with `profiles_make.txt`'s saves, as the Challenge Series needs a profile).  The track labels are those of
`tools/mw/census.py`, which reads the lists off `content_probe.txt`'s runs (every Custom Race track list stepped
once around with `NFSU2_FNG_LOG=3`) and names them from the game's English.bin.  With no profile loaded, Quick Race
offers 19 of its 137 tracks (the list draws its Accept button); the others show "This track is locked" and take no
Enter.

The career (career saves made by these runs stay outside the repository, ../seed/README.md): `career_new.txt` (a
new alias loads to a career menu with Start Career; seeded with `profiles_make.txt`'s saves), `career_start.txt`
(a new alias's career from Start Career through the prologue to the first free drive, saved),
`career_resume.txt` (that save loaded after a restart, back into free roam; seeded with `career_start.txt`'s save),
`career_safehouse.txt` (from that save: the drive into the first safehouse, the Blacklist intro and screens, every
safehouse menu item, a save, the free-roam pause menus before and after the Blacklist, the world map, Jump to Safe
House; saved), `career_safehouse_load.txt` (that save loaded back into the safehouse), `career_shop.txt` (from the
safehouse save: a customization shop, every category entered and left, a spoiler bought; saved),
`career_shop_load.txt` (the spoiler still on the car after a restart) and `career_carlot.txt` (from the safehouse
save: the car lot's 32 cars shown in turn; the car tables' check at the profile's load, runtime/mw/cars_mw.c).

The career bot (runtime/mw/bot_mw.c, TRACKER M4-15; test-only): `bot run N` plays the career from where the save
left it until the Blacklist bin is N, through the game's own paths only: it reads the bin's gates the way the
Blacklist screen reads them (races won, milestones, bounty, and what the rival asks), engages the bin's races at
their own start (the engage trigger stands at the race's start grid), lets the game's AI race them from their GO (a
car the AI wedges is put back on the road by the game's RESET, `carreset`), earns milestones and bounty in pursuits
(the AI drives about until the cops start one; Game.ForcePursuitStart(1) after 60 s of game time without one), then
engages the rival's challenge at its first race's start (InGameRivalChallenge.fng: Enter accepts), takes two of the
beaten rival's markers (SafehouseMarkers.fng), and answers the screens on the way (results, text messages, a bust's
fine, the safehouse menu); it stops at GameOver.fng (the only car impounded after its third bust) and at a career
menu it did not ask for;
`bot save` saves at the safehouse (Jump to Safe House, Save), `bot gates` logs the gates and the bin's races, `bot
race I` and `bot pursuit` do one race or one pursuit, `bot wait [N]` waits for the bot to stop.  `career_bot.txt`
runs it from a safehouse save to the bin `BOT_BIN` and saves there (its save tree is the next seed);
`bot_probe.txt` runs two single steps, `BOT_A` then `BOT_B` (for example `run 15`, then `race 0` or `pursuit`).
Run them with `NFSU2_SUITE_LIVE=1` to follow the log while they run.

Pursuits: `pursuit_start.txt` (from `career_start.txt`'s save: free roam, a pursuit started with the game's own
Game.ForcePursuitStart, the game's AI driving, waited on to its end, which the game decides: evaded or busted) and
`pursuit_heat.txt` (the same save: Game.ForcePursuitStart(2) raises the heat to 2, and the pursuit holds it in its
heat range, the Blacklist bin's; that save's bin keeps it at 1).

The Speedbreaker: `speedbreaker.txt` (a Custom Race circuit: Right Ctrl on and off, G on, the drain to the game's
switch-off, the refill above 100 mph) and `speedbreaker_pad.txt` (the pad plugged in at the start: its Y on and off).

Crash fixes (runtime/mw/fixes_mw.c): `fixes_poke.txt` reaches the null guards of the cop unload (0x57D105) and of
Attrib::StringToLowerCaseKey (0x4549BF) by poke, in the front end; with `NFSU2_TEST_COPLIST_NULL=1` and
`NFSU2_TEST_ATTRIB_NULLKEY=1` it runs the game's code instead.

Graphics, on the play page (test/play.html, with the WebGPU renderer; the pictures stay out of the repository):
`race_shots.txt` (a Custom Race circuit dumped at GO, after 300 and after 900 frames, the shadow options logged),
`race_perf.txt` (the same race driven 1800 frames between `log race_loaded` and `log drive_end`, no dumps: the page's
`&bench&gputime` window logs the frames, encode and GPU milliseconds) and `options_advanced.txt` (the Video options and
their Advanced page row by row: Shadow Detail, FSAA and Texture Filtering as offered, their maxima logged); each from
the registry it is given, `../seed/registry_max.txt` for every video option at its highest.
