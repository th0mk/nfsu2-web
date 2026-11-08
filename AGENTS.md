# AGENTS.md

How this repository is put together and the rules every change follows. Written for AI coding agents and people
alike: read it before you change code, and keep it true when you change how things work.

## What this is

*Need for Speed: Underground 2* (2004, PC) running in the browser. A Python lifter translates the game's x86 code to C
ahead of time; the C is compiled to WebAssembly together with a hand-written runtime that reimplements Windows,
Direct3D 9, DirectSound and DirectInput and adds every new feature. A TypeScript page hosts the module (WebGPU, Web
Audio, workers on shared memory) and a Rust server serves it and runs multiplayer. The same tree also builds a native
macOS app (secondary). Players bring their own copy of the game.

## Layout

| Path | What |
|---|---|
| `tools/` | The lifter (`lift.py`, `genall.py`, Capstone) and the pack builders (languages, textures, mods) |
| `gen/` | Generated from `SPEED2.EXE` by `web/gen.sh`. Never edit, never commit |
| `runtime/` | Hand-written C: the Windows/D3D9/sound/input layer and every game feature. `runtime/web/` = browser backends, `*.m` = macOS backends |
| `web/src/` | The page: `app/` start page and in-game overlays, `host/` the wasm host, `gpu/` WebGPU, `audio/`, `input/`, `net/` |
| `web/server-rs/` | The server, a Cargo workspace: `crates/http`, `roam` (free roam hub), `online` (EA Online lobby, buddies, stats), `lan`, `claims`, `bans`, `vault`, `util`, … |
| `web/test/` | Page, server and C unit tests (`node --test`) |
| `tests/` | C unit tests and harnesses; `tests/scripts/*.txt` are input scripts that drive the real game |
| `discord-bot/` | The Discord forum bot |

## Never commit

- Anything made from the game: `gen/`, `out/`, game files, extracted assets, screenshots of game content.
- Personal details: players' names or handles, logs tied to a person, server addresses or hosting details.

## Product rules

These apply to every change, whatever the area.

- **The original game by default.** Don't change handling or physics, car performance, AI or traffic, career
  progression, rewards or the economy. Multiplayer, quality of life, controls, languages and fixes of the original's
  bugs are welcome. Graphics enhancements are opt-in; the default look is the original's, except sharper copies of
  it on computers (Upscaled HUD, anti-aliasing, anisotropic filtering, HQ vinyls).
- **The single-player career stays available as the original at all times.** A multiplayer feature that touches it
  is something the player chooses.
- **Saves stay valid in the stock PC game.** Anything extra goes in a file beside the profile (as `GARAGE.DAT` and
  `MODPARTS.DAT` do), forward compatible: unknown entries carried, a newer file never overwritten.
- **Few settings.** Prefer a good default over a switch. A runtime knob gets a line in the settings.ini template, not
  a row on the page, unless it is a real player choice. Every UPPER_CASE settings.ini key reaches the game's
  environment, so a test-only knob must be gated (only while `NFSU2_INPUT_SCRIPT` is set).
- **Plain player-facing text.** Short, everyday words, the one thing to do; no technical terms unless the player has
  to type them. Every page text is translated (see the page section).
- **No AI-generated voices, sound or video.**

## Runtime (C)

### How a feature attaches to the game

1. **Override a lifted function:** define `sub_XXXXXX(CPU *c)` in a runtime file and call `sub_XXXXXX_orig(c)` for
   the game's own code. One override per function: when two features need the same function, the override's owner
   calls into the other.
2. **Instruction hook:** a `case HOOK_SITE(0x...)` in the feature's `*_hook(CPU *c, uint32_t ip)`.
3. **Per-frame work:** a `*_frame` function.

`gen.sh` finds overrides and hook sites by scanning `runtime/*.c` and `*.m` for `_orig` names and `HOOK_SITE`, so
they must stay literal in the source (no macros that build them), and adding or removing one needs a new lift
(`web/gen.sh <SPEED2.EXE>`); `web/build.sh` refuses a stale one.

Register a feature in `runtime/features.c`, which has three lists, each run in its own order: the installs at boot
(with the builds that run them), the instruction hooks (the first that takes an address ends the chain) and the
per-frame calls. Declare the functions in `runtime/featurelist.h` (not `features.h`: that name is a C library header).

### Shared helpers, not local copies

- Guest memory: `runtime/guest.h`: `guest_ok` / `guest_ok_bytes` (range checks), `guest_rd32`, `guest_read_str`,
  `guest_write_str`, and `gcall` / `gcall_main` / `guest_call_ex` to call a game function (the return address pushed
  matters: some game functions check their caller's).
- Game addresses and struct offsets: `runtime/game.h`, each named once. Add to it instead of a local `#define`.
- Settings from the environment (`NFSU2_*`): `setting_bool` and `ENVC` in `runtime/setting.h`.
- Don't add to `runtime/cpu.h` unless the lifted code needs it: it is part of the lifted shards' build hash, so any
  edit recompiles all of them.

### Build constraints

- A new `.c` file goes into every build list: `web/build.sh`, `build.sh`/`link.sh` (macOS) and the tests that build
  runtime files (grep for an existing file name).
- `roam_core.c` and `roam_game.c` are compiled with `-ffp-contract=off` (by name, in `web/build.sh` and `link.sh`):
  their results must match the Windows plugin bit for bit. A new file split from them goes into those lists.
- `runtime/roam_game.c` is also built as a Windows plugin with no C library (`tools/build_roam_asi.sh`): use only
  what `roam_host.h` provides there. `tools/roam_fingerprint.py` reads its `FN_`/`G_` defines.
- Guest memory is sparse: check a range before reading it. The game's executable never changes, so fixed addresses
  are fine, named in `game.h`.

### Style

- Logs: `fprintf(stderr, "[tag] ...")` with the feature's tag, in English.
- Comments explain what an address or offset is and why the code does what it does; keep a measurement next to the
  number it justifies. No history ("used to", "now"): that is what git is for.

## Page (TypeScript)

- Plain DOM, no framework. A panel is a class built as `new X(root, deps)` with its callbacks injected; it only
  queries inside its own root. `app/shell.ts` wires them together.
- `strict` TypeScript: `npm run check` must pass. Browser types TypeScript lacks go in `web/src/dom-extras.d.ts`, not
  `as any`.
- Text: English is the key. Wrap every text in `t('…')`, `tn()` or `N('…')` with a literal, then translate it in every
  `web/src/app/lang/*.json` (`node web/tools/i18n-keys.mjs --check`; `web/test/i18n.test.mjs` fails otherwise).
- Browser storage goes through `localStore`/`sessionStore` in `app/prefs.ts`, which never throw; every key is a named
  constant there, and a stored key's spelling never changes (players' choices are kept under it).
- settings.ini is read and written through `SettingsFile` (`app/settings-store.ts`); the settings table and the pure
  text edits are in `app/settings.ts`.
- Messages to and from workers are typed in each `*/proto.ts`.
- Look: reuse the existing classes and tokens in `app/style.css`. Every margin, padding and gap is a multiple of 4 px;
  controls are 40 px tall with 8 px corners, panels and dialogs have 12 px corners.

## Server (Rust)

- The players' virtual LAN is a switch (`lan`). The free roam hub (`roam`) and the Online host (`online`) are hosts
  on it: each runs on a thread of its own behind `nfsu2_util::host::Host`, and the switch posts it its frames in order.
  Network I/O runs on tokio.
- Reuse what exists: `nfsu2_util::atomic_file` for every file written whole, `nfsu2_util::bucket` for rate limits,
  `nfsu2_util::log` for logs, and the `nfsu2-anticheat` crate for the race checks free roam and Online share.
- Lints are set once in the workspace's `[workspace.lints]`; a crate opts in with `[lints] workspace = true`.
- Never panic on a player's input: protocol handlers count a bad frame and drop it.
- Limits are defaults in the code, not new environment variables.
- Tests drive the hosts through their public API (`crates/roam/tests`, `crates/online/src/tests`), with fake clocks
  where time matters.

## Before you commit

| Changed | Run |
|---|---|
| `web/src/` | `cd web && npm run check && npm run test:fast` |
| `web/server-rs/` | `cargo fmt --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test -p <crate>` |
| `runtime/` | `web/build.sh`, `cd web && npm run test:fast`, then game scenarios (`node web/tools/run-suite.mjs --list`) against a dev server: `out/web/server-rs/nfsu2-server --data <game folder> --http-port 8611 --dev` |

- One logical change per commit, with a short title: `Area: what changed`.
- `CHANGELOG.md` is written per release, for changes players notice.
