# Most Wanted input scripts

Input scripts for runtime/autotest.c, served at `/tests/scripts/` by a server started with `--game mw --dev`
(see ../README.md).

Besides autotest.c's commands, Most Wanted's runs take `call ADDR [ecx=X] [ARG ...]`, `loadlib NAME`, `objects`
(the kernel objects in use by kind and creator), `movie NAME` (a movie played as the front end plays one),
`movieup` and `movieend` (wait until it is up, then gone) and `waitgone PACKAGE` (runtime/mw/debug_mw.c), all
test-only, and the race aids of runtime/mw/career_mw.c, test-only too, which run at
the game's per-frame anchor: `racewin` (a shortcut finish: the player is finished where it is, with three of the
calls the race's script makes at the line), `autopilot on|off` (the game's AI drives the player's car), `autopilot
races` (the same at every race's GO from then on, for races that follow each other with no menu between, with each
race's start and end logged; a lost race is raced again as a player would: Enter on its results, then Resume Career),
`teleport X Y Z DX DY DZ`, `where`, `racestatus` and `frames`.

The races: `race_frames.txt` (a Custom Race circuit long enough for it, the anchor counted over 10,000 frames of
the race), `race_loop.txt` (25 Quick
Races in a row, each ended by `racewin`, with the kernel objects before each load) and `race_win.txt` (a Custom Race sprint the AI-driven car wins over the
line, saved; seeded with `profiles_make.txt`'s saves).

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
(a new alias's career from Start Career through the prologue to the first free drive, saved) and
`career_resume.txt` (that save loaded after a restart, back into free roam; seeded with `career_start.txt`'s save).
