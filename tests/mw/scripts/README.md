# Most Wanted input scripts

Input scripts for runtime/autotest.c, served at `/tests/scripts/` by a server started with `--game mw --dev`
(see ../README.md).

Besides autotest.c's commands, Most Wanted's runs take `call ADDR [ecx=X] [ARG ...]` and `loadlib NAME`
(runtime/mw/debug_mw.c), both test-only, and the race aids of runtime/mw/career_mw.c, test-only too, which run at
the game's per-frame anchor: `racewin` (a shortcut finish: the player is finished where it is, with three of the
calls the race's script makes at the line), `autopilot on|off` (the game's AI drives the player's car), `teleport X Y
Z DX DY DZ`, `where`, `racestatus` and `frames`.

The races: `race_frames.txt` (a Custom Race circuit long enough for it, the anchor counted over 10,000 frames of
the race), `race_loop.txt` (25 Quick
Races in a row, each ended by `racewin`) and `race_win.txt` (a Custom Race sprint the AI-driven car wins over the
line, saved; seeded with `profiles_make.txt`'s saves).

The frame rate: `menu_hold.txt` (the main menu's camera swing-in traced, then the menu held for 60 s, a `[status]`
line over it and the clocks in peeks; its check wants the swing over in 6 s of wall time and RealTimeFrames at 60 a
second at the page's frame-rate setting `NFSU2_FPS`, runtime/mw/framerate_mw.c; `NFSU2_HFR_LOG=1` adds a `[mwpace]`
line a second with the slots and the steps the simulation ran).

The race kinds (TRACKER M4-05, M4-06): `race_kind.txt` (a Custom Race circuit, sprint, lap knockout or speedtrap on
one track, picked on the screens by the kind's menu item and the track's name label, `MWK_RUNS` times, each ended by
`racewin`), `race_drag.txt` (a Custom Race drag the game's AI drives to its end) and `race_tollbooth.txt` (the
Challenge Series' first event, a Tollbooth Time Trial: Quick Race has no tollbooth; seeded with `profiles_make.txt`'s
saves, as the Challenge Series needs a profile).  The track labels are those of `tools/mw/census.py`, which reads
the lists off runs made with `NFSU2_FNG_LOG=3` and names them from the game's English.bin.  With no profile loaded,
Quick Race offers 19 of its 137 tracks (the list draws its Accept button); the others show "This track is locked"
and take no Enter.
