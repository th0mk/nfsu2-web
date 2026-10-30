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

The player's page (web/tools/app-test.mjs --game mw, not run-suite): `app_rw_probe.txt` (a first start makes and
saves the alias ROTEST, then a Custom Race to GO and 600 frames of it, quit in the race) and `app_load_probe.txt` (the
one alias of the save folder loads at the start, then the main menu: after a restore, an import or a rename).

The movies: `intro_idle.txt` and `intro_title.txt` (the boot movies and the title, for web/tools/intro-test.mjs
--game mw), `movies.txt` (all 32 movies of the game folder played to their end and skipped; with `--name
movies_all_av1`, from a game folder with the AV1 copies, its check also wants every play through the AV1 hook),
`movies_av1.txt` (the boot movies and blacklist_15 to their ends, dumped on the play page; its check wants each play
through the AV1 hook, `--name movies_vp6` the VP6 decoder's: a browser without AV1, `NFSU2_MOVIE_CODEC=vp6`, or a folder
without the copies; with `NFSU2_LANGUAGE` another language, whose movies are the English ones),
`media_refshots.txt` (the play page's pictures of the movie screens and the new alias's name entry) and
`credits.txt` (Options > Credits rolled to their end).

The languages: `lang_menu.txt` (TRACKER M7-13: the title and the main menu in the language `NFSU2_LANGUAGE` names,
dumped on the play page; with `FILELIST=1` its check wants the registry's Language, one of the game's ten, and that
language's `LANGUAGES\<NAME>.BIN` read, no other's).

The mouse: `mouse_menu.txt` (TRACKER M3-08: the main menu's wheel takes the highlight to Options, clicks on the
selection frame open its sub menu and the options screen, twice, the second time with the windowed flag set; run it
with `NFSRES=800x600` and `1024x768`), `mouse_menu_169.txt` (the same walk at 16:9, `NFSRES=1280x720` and
`1920x1080`) and `mouse_menu_219.txt` (at 21:9, `NFSRES=2520x1080`, TRACKER M7-03: the front end stays 4:3 in the
middle of a wider screen, so its objects sit at other window fractions).  Until the page's pointer first moves, Most
Wanted sees the cursor outside its window (game.h cursor_away): its front end highlights the object a still cursor
rests on.

The screen's shape (TRACKER M7-01..M7-06, runtime/mw/widescreen_mw.c): `wide_shots.txt` dumps the attract movie, the
title, the main menu, the car list and a race (at GO, the chase view, the hood view with the rear-view mirror) on the
play page; run it at each canvas shape (`w`, `h`: 4:3, 16:9, 21:9, 32:9 and upright).  `title_shots.txt` dumps
the title alone (movies off), the 4:3 one or with `NFSMW_WIDE_SPLASH=1` the game's widescreen one.
`wide_resize.txt` and `wide_resize50.txt` change the canvas under the game at each "resize" line (the play page's
`&resizeon=<w>x<h>,...`): twice, or 50 times through five shapes with a `[status]` line after each, whose check wants
the texture ids, kernel objects, file handles and threads at the end as after the first round.

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
Blacklist screen reads them (races won, milestones, bounty, and what the rival asks), takes the career's best car
(the Car Lot's tier, then the package levels on it, then the latest won) in the safehouse's Car Select (`bot run N
keep` keeps the current one; the list is not always in career-record order, so a place an Enter showed holding
another car is remembered and the pick tried again), buys performance packages for that car at a customization shop
with the career's cash (engine, turbo, transmission, nitrous and tires first; the dearest level of each package above
the car's that the shop takes, once a bin and again when the cash has grown), engages the bin's races at their own engage trigger, lets the
game's AI race them from their GO (a lost race is raced again in its turn; a car the AI wedges is put back on the
road by the game's RESET, `carreset`), earns milestones and bounty in pursuits (the AI drives about until the cops
start one; Game.ForcePursuitStart at the bin's top heat after 60 s of game time without one; a chase lost too early
is rejoined where it was, and one that ran 300 s or boxed the car in is broken off by setting the car far away, so
the game ends it as evaded), then engages the rival's challenge at its first race's start
(InGameRivalChallenge.fng: Enter accepts; a lost one again, after the shop when the cash reaches a package, and after
two lost in one car in the other of the two best cars, taken at the safehouse), takes two of the beaten rival's markers
(SafehouseMarkers.fng; what they gave is logged from the cash, the cars and the markers held by type;
`NFSMW_BOT_MARKERS=X,Y` takes the cards X and Y by their objects, 0xCDA0A66B to 0xCDA0A670 from the left, instead of
the first two), and answers the screens on the way (results, text messages, the safehouse menu).  A car at a race's
start already (a lost lap knockout ends there) is set at another race's place first: from inside the engage trigger
the Enters opened nothing.  After a bust it pays the fine with cash
(Infractions.fng), but when the strike would impound the car it spends an Out of Jail marker if it holds one; after an
impound it takes a car that can be driven in Car_Select.fng (M4-48).  It stops at GameOver.fng (no car left that can be
driven) and at a career menu it did not ask for;
`bot save` saves at the safehouse (Jump to Safe House, Save), `bot gates` logs the gates and the bin's races, `bot
race I` and `bot pursuit` do one race or one pursuit, `bot bust N [pay]` lets the cops bust the standing car N times
(`pay`: no marker spent), `bot car` takes the other of the two best cars at the safehouse (as after two lost rival challenges;
bot_probe.txt with `BOT_A=car BOT_B=car` from bin11_start does it from the safehouse menu, then from free roam),
`bot wait [N]` waits for the bot to stop.  `bot_impound.txt` runs `bot bust ${BUST_N}
${BUST_PAY}` from a safehouse save (bin13_start: the third bust spends a marker, or with `pay` impounds the first car
and the bot takes the second; bin15_start with `pay`: game over, the bot stops there).  `career_bot.txt`
runs it from a safehouse save to the bin `BOT_BIN` and saves there (its save tree is the next seed);
`bot_probe.txt` runs two single steps, `BOT_A` then `BOT_B` (for example `run 15`, then `race 0` or `pursuit`);
`bot_segment.txt` runs one (`BOT_A`, for example `run 13 races`: the bin's races, then stop; `run 14 races 2`: two
of them; `run 12 ready` from bin 13: until the rival's requirements are met) and saves at the safehouse, a seed for the next
segment.  A car the AI wedges is reset the game's way, but not in a pursuit: a reset with cops on the car ends in the
bust.
Run them with `NFSU2_SUITE_LIVE=1` to follow the log while they run.

The Blacklist's regression scripts (TRACKER M4-13; bugs other ports reported, FEATURES F-004), each seeded with one of
the career bot's seeds: `career_bl15.txt` (Sonny beaten from bin15_ready: his stats screen, the markers, the next
rival's bio in order, none stuck, and the counter at 14; codepdbh #84), `career_bl15_race3.txt` (the third race of
bin 15 from bin15_two, two races won: engaged, 15 % in, won; #112) and `career_bl14.txt` (the rival's race of bin 14
from bin14_ready: 15 % in, won, the bin at 13; #162).  `career_unlocks.txt` (a probe, with NFSU2_FNG_LOG=3) shows a
career save's Quick Race track lists and its car list (M4-44), `career_rapsheet.txt` (the same) its Rap Sheet's
screens one after another.

Pursuits: `pursuit_start.txt` (from `career_start.txt`'s save: free roam, a pursuit started with the game's own
Game.ForcePursuitStart, the game's AI driving, waited on to its end, which the game decides: evaded or busted) and
`pursuit_heat.txt` (the same save: Game.ForcePursuitStart(2) raises the heat to 2, and the pursuit holds it in its
heat range, the Blacklist bin's; that save's bin keeps it at 1).  `pursuit_rap.txt` (from a career save past the
prologue: the rap sheet at the safehouse, the career bot's `PR_BOT`, `pursuit` for one pursuit or `chase S` for
pursuits one after another for S game seconds, the AI driving each to the end the game decides, then the rap sheet
again and a save) and `pursuit_load.txt` (that save loaded after a restart: its rap sheet must equal the saved one).
Their aids (runtime/mw/pursuit_mw.c, test-only): `rapsheet` (the rap sheet's ten values, the best pursuit's, their
Most Wanted ranks, the cash, the impound markers and each career car's record with its impound strikes), `icons [T]`
(the world map's icons, those of type T one by one; `icons go T K DX DY DZ` moves the car to one), `pools [watch]`
(the game's memory pools and frame arena, with their peaks); `pursuit watch` also logs the counts the game keeps by
name (helicopters, roadblocks and spike strips dodged, cops damaged and destroyed), roadblocks put up and pursuit
breakers come down (hooks, with the car's place at each breaker), how often the purecall guard's call ran and, in the
cops' search, its meter and parts (the search's seconds, the bonus, its length); each cop car the cop manager asks for
by name, and at each pursuit's end the cars asked for by name and (under `pools watch`) the pools with their peaks.
`NFSMW_BOT_PLOG=S` makes the bot log the car's place every S game seconds in a pursuit (15 by default);
`NFSMW_BOT_SEARCH=stay` leaves the cops' search alone (the game's own cooldown) and `NFSMW_BOT_SEARCH=hide` sets the car
in the nearest hiding spot at its start, standing (default: back to the chase); `NFSMW_BOT_FHEAT=N` forces pursuits at
heat N instead of the bin's top heat (1: the game's escalation raises it).  `pursuit_breaker.txt` drives the car at the one pursuit
breaker the AI has brought down so far (TRACKER M4-17; not working yet: its header says what was seen).

Sound: `menu_audio.txt` (the front end's sound set-up and the mixer's pace at the main menu) and `shop_music.txt`
(from a career save past the prologue: free roam, the customization shop entered and left 50 times; with READLOG=1
the check wants the game to read the music, MW_Music.mus, again after every visit; TRACKER M4-26) and
`tunnel_pass.txt` (from a career save past the prologue: 20 drives through a tunnel, the game's own tunnel state and
reverb switch logged with NFSMW_TUNNEL_LOG=1 by runtime/mw/sound_mw.c; TRACKER M4-27).

The Speedbreaker: `speedbreaker.txt` (a Custom Race circuit: Right Ctrl on and off, G on, the drain to the game's
switch-off, the refill above 100 mph) and `speedbreaker_pad.txt` (the pad plugged in at the start: its Y on and off).
`breaker_ai.txt` holds G from 10 frames after the AI has driven and let go (TRACKER M4-47: a press in the first frame
or two after Game.ClearAIControl is lost, and a key held from then on makes no new press), with what the game's CanUse
says; `breaker_ai_taps.txt` taps G twelve times from `BA_FIRST` frames after the AI let go (`BA_AI=off`: the control
without the AI), and `breaker watch` logs each GAMEBREAKER action the player's InputPlayer reads and its action queue's
state.

Crash fixes (runtime/mw/fixes_mw.c): `fixes_poke.txt` reaches the null guards of the cop unload (0x57D105) and of
Attrib::StringToLowerCaseKey (0x4549BF) by poke, in the front end; with `NFSU2_TEST_COPLIST_NULL=1` and
`NFSU2_TEST_ATTRIB_NULLKEY=1` it runs the game's code instead.

Graphics, on the play page (test/play.html, with the WebGPU renderer; the pictures stay out of the repository):
`race_shots.txt` (a Custom Race circuit dumped at GO, after 300 and after 900 frames, the shadow options logged),
`race_moments.txt` (the same race dumped at set race-timer moments: GO, 0.03, 0.18, 0.30, 1 and 3 s with the
player's car still on the start line, then 8 and 12 s with the AI driving; checked by ../checks/race_moments.sh;
`&dumptex=1,2` adds the shadow map's colour and depth),
`race_perf.txt` (the same race driven 1800 frames between `log race_loaded` and `log drive_end`, no dumps: the page's
`&bench&gputime` window logs the frames, encode and GPU milliseconds) and `options_advanced.txt` (the Video options and
their Advanced page row by row: Shadow Detail, FSAA and Texture Filtering as offered, their maxima logged); each from
the registry it is given, `../seed/registry_max.txt` for every video option at its highest.

Free roam with friends (TRACKER M10-04 to M10-07, M10-25; runtime/mw/roam_mw.c, NFSU2_ROAM=1): each starts from the
career saved in free roam (the freeroam career seed, ../seed/README.md) and resumes it into free roam.
`roam_probe.txt` (run-suite, one page, no network: `roam probe` logs the player's car's rigid body, matrix rows and
controls, standing and with LEFT and UP held; `roam spawn [NAME]` makes cars of our own ahead of it the way friends'
cars are made, posed every frame; `roam pools` logs what they use up; `ROAM_SPAWN` names the second car's vehicle;
`roam unspawn` takes them away, two are made and taken away again, and the check wants the game's vehicles of driver
class 4 back to none of ours after each unspawn).
`roam_mp_drive.txt` and `roam_mp_watch.txt` are web/tools/roam-pair.mjs --game mw's pair: A's car driven by the game's
AI (`autopilot on`), B waiting for A's car to have moved 50 m (`roam mp waitpeers`, `waitcars`, `waitmoved N M`, `roam
mp status`); roam-pair compares where B shows A's car with where A was (tools/roam_compare.py).
`roam_crowd_probe.txt` is the pools probe (M10-05): one page with web/tools/roam-crowd.mjs --game mw's simulated players
(give the crowd `--track` a pose log of a real drive, such as the start of the pair's A.pose.csv, so the cars stand on
the road), `[roampool]` lines every 600 frames; the game's car loader takes six friends' cars beside the player's.  Then
`roam mp drop` takes the friends' cars away and they are made again (a friend's car taken away and made anew).
