# Need for Speed: Underground 2 in the browser

A fan project that runs the PC game *Need for Speed: Underground 2* (2004) in Google Chrome and Firefox. The game's
x86 code is translated to C ahead of time and compiled to WebAssembly. Direct3D 9 runs on WebGPU and the sound on
Web Audio. You bring your own copy of the game.

> Not affiliated with or endorsed by Electronic Arts. This repository contains none of the game's code or data.

![The start page: Play, the controls, your saves and Online](docs/web/start-page.png)

## What you need

- **The game:** your own PC copy, patched to v1.2 (North America), with this exact `SPEED2.EXE` (other versions
  will not work): 4,800,512 bytes, SHA-256 `f9dd86c054878ce6276beb07c1fd61874f7a1e4bf1f241b084c65b73e24168a7`.
- **To build:** an Apple Silicon Mac (the tested setup) or Linux, Python 3, Node.js 20 or newer, Rust
  ([rustup](https://rustup.rs)), and [wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases) 34 for that system
  unpacked into `deps/`. On Windows,
  build in WSL 2 with the clone in the Linux file system (`~/nfsu2-web`): under `/mnt/c` it builds much slower.
- **To play:** the latest Google Chrome, or Firefox where it has WebGPU (Windows, macOS).

## Build

```sh
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
web/gen.sh "/path/to/Need for Speed Underground 2/SPEED2.EXE"   # translate the game's code to C (into gen/)
web/build.sh                                                    # compile it to out/web/nfsu2.wasm
```

The generated C and the compiled module are made from the game, so they are never committed (`.gitignore`).

## Play

```sh
web/start.sh --data "/path/to/Need for Speed Underground 2"
```

Open **http://localhost:8080/** in Chrome or Firefox and press **Play**. The start page has the settings, your saves
and the controls. The player's guide is [docs/web/PLAYING.md](docs/web/PLAYING.md). The game plays in any of its
thirteen languages, in Brazilian Portuguese, in Polish or in Turkish; the dubbed voices and movies and the Portuguese,
Polish and Turkish text come from mods, as language packs ([docs/web/VPS.md](docs/web/VPS.md#languages)). The menus and
HUD can be redrawn at four times their size, from a mod as a texture pack ([docs/web/VPS.md](docs/web/VPS.md#texture-packs)).

To play with friends, run the server behind HTTPS, for example on a small VPS with Docker:
[docs/web/VPS.md](docs/web/VPS.md).
Everyone who plays from the same server can meet in free roam.

## How it works

- **`tools/`:** a Python lifter (Capstone) turns every function of `SPEED2.EXE` into C.
- **`runtime/`:** Windows, Direct3D 9, DirectSound and DirectInput, reimplemented. `runtime/web/` holds the browser
  parts.
- **`web/src/`:** the page. It runs the game's threads as workers on one shared memory, draws with WebGPU and plays
  sound through an AudioWorklet.
- **`web/server-rs/`:** the server, in Rust: serves the page, the module and your game folder, and connects players
  for multiplayer.

More in [docs/web/DESIGN.md](docs/web/DESIGN.md). The same tree still builds the native macOS version
(`./gen.sh`, `./build.sh && ./link.sh`, `./run.sh`).

## Credits

Based on Talha Müderrisoğlu's static recompilation of NFSU2 for Apple Silicon, which in turn follows the method of
[N64: Recompiled](https://github.com/N64Recomp/N64Recomp) and [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).
The language packs are built from KyoriAsh's [Multi 13 languages mod](https://nfsmods.xyz/mod/5749) (with Lee-Ryo's
Thai support, whose patch `runtime/language.c` follows), from TraduMix's and MixMods' Brazilian Portuguese
translation (Fyro, Beater, Kerlyston F.; Junior_Djjr, SideWinder, Elyzandro), from PLProjekt's Polish translation
(Daniel Graszka, MaDeR, Marcin Piech, Nati, Mateusz Wilkowski, alter), from VileStuff's
[English Subtitles Fixed](https://nfsmods.xyz/mod/5286) and from muntazam's
[Turkish translation](https://www.moddb.com/mods/need-for-speed-underground-2-turkish-translation-patch). The texture pack is built from LunicAura106's
[Upscaled HUD](https://nfsmods.xyz/mod/4319). Built with the help of Claude Code.

## License

MIT ([LICENSE](LICENSE)). *Need for Speed* and *Need for Speed: Underground 2* are trademarks of Electronic Arts Inc.
