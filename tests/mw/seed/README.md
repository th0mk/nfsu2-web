# Most Wanted registry seeds

Registry seeds (`registry.txt` files) that runs start from, served at `/tests/seed/` by a server started with
`--game mw --dev` (see ../README.md); `node web/tools/run-suite.mjs --game mw --reg /tests/seed/<file> ...` starts
every scenario of the run from one (without it a run starts from the game's own first start). Career saves stay
outside the repository.

- `registry_first.txt`: the registry the game writes at its first start in a headless run: `FirstTime` 1 and the
  settings its detection chose. With `FirstTime` set the start loads the stored settings (LOAD_SETTINGS) instead of
  detecting them, so a run from it starts with these values on any machine; its log shows
  `[reg] ...\FirstTime = 1 (REG_DWORD, stored)` and each `g_*` value `stored`.
- `registry_max.txt`: every video option at the highest the Video options offer on this renderer (a first start
  stores lower ones): Shadow Detail 2, FSAA 1 (the maximum, CheckDeviceMultiSampleType's 2 quality levels - 1),
  Texture Filtering 2 (anisotropic), rain, the highest world and reflection detail; for the play page's graphics
  scripts (`race_shots.txt`, `race_perf.txt`, `options_advanced.txt`).
- `registry_max_fsaa0.txt`: `registry_max.txt` with FSAA 0, the other half of an FSAA off/on pair (`race_moments.txt`).
- `registry_fx_envmap0.txt`, `registry_fx_roadrefl0.txt`, `registry_fx_blur0.txt`, `registry_fx_overbright0.txt`,
  `registry_fx_particles0.txt`, `registry_fx_rain0.txt`, `registry_fx_vt0.txt`: `registry_max.txt` with one effect
  off (the car's environment map, the road reflection, motion blur, overbright, particles, rain, the visual
  treatment), the other half of an effect's on/off pair (`fx_census.txt`, `fx_rain.txt`, `fx_shots.txt`).
- `registry_phone.txt`: `registry_max.txt` with a phone's graphics (web/src/games/mw/registry.ts PHONE_GRAPHICS: the
  environment map, road reflection, motion blur and overbright off); `registry_phone_vt0.txt` with the visual
  treatment off as well.

A career save tree (NFSU2_SAVE_SEED) holds the registry the game wrote next to the saves (`.nfsmw_registry`), so a
run seeded with one needs no registry seed. The career seeds, made by these scripts and kept outside the repository:

| Seed | Made by | Loads to (checked by) |
|---|---|---|
| a new alias, no career | `profiles_make.txt` (ALPHA and BRAVO) | the main menu; Career offers Start Career (`career_new.txt`) |
| the first free drive | `career_start.txt` (the prologue played; the free roam's autosave) | free roam, the car where the career's free roam begins (`career_resume.txt`) |
| the first safehouse | `career_safehouse.txt` (the Blacklist intro seen, saved in the safehouse menu; Cash 3,000) | the safehouse menu, bin 15 (`career_safehouse_load.txt`); `career_shop.txt` and `career_carlot.txt` start from it |
| a part bought | `career_shop.txt` (the Reaper spoiler, saved in the safehouse; Cash 2,000) | the safehouse menu; the shop's spoilers on the Reaper (`career_shop_load.txt`) |
