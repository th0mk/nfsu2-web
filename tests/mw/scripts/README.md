# Most Wanted input scripts

Input scripts for runtime/autotest.c, served at `/tests/scripts/` by a server started with `--game mw --dev`
(see ../README.md).

Besides autotest.c's commands, Most Wanted's runs take `call ADDR [ecx=X] [ARG ...]` and `loadlib NAME`
(runtime/mw/debug_mw.c), both test-only, and the race aids of runtime/mw/career_mw.c, test-only too, which run at
the game's per-frame anchor: `racewin` (a shortcut finish: the player is finished where it is, with three of the
calls the race's script makes at the line), `autopilot on|off` (the game's AI drives the player's car), `teleport X Y
Z DX DY DZ`, `where`, `racestatus` and `frames`.

The races: `race_frames.txt` (one Quick Race, the anchor counted over 10,000 frames), `race_loop.txt` (25 Quick
Races in a row, each ended by `racewin`) and `race_win.txt` (a Custom Race sprint the AI-driven car wins over the
line, saved; seeded with `profiles_make.txt`'s saves).
