# Building and testing the NFSU2 NSP ("fullbuild")

The NSP is one installable title (`01004E4653550000`): the program as an NSO plus `nfsu2.dat` in its RomFS, signed the way homebrew NSPs are.
At start `vfs_dat_mount_romfs` (runtime/switch/vfs_dat.c) finds `nfsu2.dat` in the root of the process's RomFS and reads it with `fsStorageRead`; an NRO has no
RomFS and falls back to `sdmc:/switch/nfsu2/nfsu2.dat`. In the NSP build online turns itself on (no `online` file) unless an `offline` file exists.

## Build

Needs Docker (image `devkitpro/devkita64`), `release/nro/switch/nfsu2/nfsu2.dat`, and three things in `out/nsp/` (git-ignored):

1. `hacbrewpack`: hacBrewPack built in the image. The original repository (The-4n/hacBrewPack) is gone; the fork used is
   `github.com/dragonflylee/hacBrewPack` (master tarball sha256 `21726242a982b702d8d000a98fe77fafd3771668029dee62db069512e7b7e03b`), built with `make` and copied to `out/nsp/hacbrewpack`
   (`runtime/switch/nsp/build-hacbrewpack.sh` does it inside the container). GPL-2.0.
2. `keys-min.txt`: ONLY the `header_key` and `key_area_key_application_*` lines of your `prod.keys` (hacBrewPack's parser rejects some other lines of a full file).
   **Never commit keys**; `out/` is git-ignored.
3. `logo/NintendoLogo.png` and `logo/StartupMovie.gif`: the "Nintendo Switch" animation shown while the title loads. They are Nintendo's files and are **not in this repo**; extract them
   from the Program NCA (section 2) of any NSP you own, e.g. `hactool -k keys-min.txt -t nca --section2dir=out/nsp/logo <program.nca>`. Without them hacBrewPack makes a black loading screen.

Then:

```
runtime/switch/build.sh nro        # the ELF (out/switch/nfsu2.elf) and the NRO
runtime/switch/build-nsp.sh        # NSO + NPDM (runtime/switch/nsp/app.json) + NACP + icon -> hacBrewPack -> release/nsp/Need for Speed Underground 2 [01004E4653550000][v0].nsp
```

`runtime/switch/nsp/app.json` is hacBrewPack's template NPDM with: title id `0x01004e4653550000`, 39-bit address space, **cores 0-2** (core 3 is the system's: a main thread on it never ran),
threads 24-63, and the SVCs the runtime uses (`CreateCodeMemory`, `ControlCodeMemory`, `CreateSharedMemory`, ...).

## Test (yuzu, before any console)

yuzu needs `network_interface=Ethernet` in `qt-config.ini` (it ships as "None": every guest `socket()` asserts) and `prod.keys` + firmware.

```
powershell -File runtime\switch\yuzu-online-test.ps1                        # an existing profile -> Online -> an Online screen (RANKINGS or the car selection)
powershell -File runtime\switch\yuzu-online-test.ps1 -Fresh -Package nro    # FROM ZERO: release\nro copied to an empty SD, new profile
powershell -File runtime\switch\yuzu-online-test.ps1 -Fresh -Package nsp    # FROM ZERO: release\nsp opened as a title, nothing on the SD
powershell -File runtime\switch\yuzu-online-test.ps1 -NegativeControl       # hub down: the test must FAIL
```

Each run prints PASS/FAIL per check with the log line as evidence and asks the local hub (WSL, `hub-keep.sh`) to confirm the join independently. `-Fresh` moves the yuzu SD folder aside and puts it back;
the last log is kept in `out/nsp/last-run-<nro|nsp|existing>.log`. Test-only flag files: `tbuf` (yuzu's shader limit), `fnglog`, `padlog`, `keepsave`, `capture`, `slowdown.txt`, `show.txt`.
A brand-new profile is sent to the Online car selection (`UI_OLCarSelect`) first, so `-Fresh` accepts that screen where the existing profile reaches `UI_OLRankings`.

## Known limits

- The NSP is validated in yuzu only; the first install on a console is the real test (signature patches needed, like any homebrew NSP).
- The NSP carries the game data: it is for your own copy of the game, not for distribution.

## Name and credits

The title is **Need for Speed: Underground 2**, author/publisher **Electronic Arts** (NACP, for both the NRO and the NSP). The game, its name and its data belong to Electronic Arts; this is an unofficial port.
