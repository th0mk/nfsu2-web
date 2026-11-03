#!/bin/bash
# build-nsp.sh -- the "fullbuild" NSP: the program (NSO) plus nfsu2.dat in its RomFS, one file to paste into DBI's installer.
#   runtime/switch/build-nsp.sh                     (after runtime/switch/build.sh nro; needs Docker, out/nsp/hacbrewpack and out/nsp/keys-min.txt)
# Inputs : out/switch/nfsu2.elf, release/nro/switch/nfsu2/nfsu2.dat, runtime/switch/nsp/app.json (the NPDM: title 01004e4653550000, cores 0-2)
# Logo   : out/nsp/logo/{NintendoLogo.png,StartupMovie.gif} -- the "Nintendo Switch" boot animation (Nintendo's files, not in this repo; docs/switch/NSP.*.md)
# Output : release/nsp/Need for Speed Underground 2 [01004E4653550000][v0].nsp
# out/nsp/hacbrewpack is hacBrewPack (dragonflylee fork) built in the devkitpro/devkita64 image (see docs/switch/NSP.md); keys-min.txt holds ONLY
# header_key and key_area_key_application_* from a prod.keys; both stay in out/ (git-ignored).  Never commit keys.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
[ -f out/nsp/hacbrewpack ] && [ -f out/nsp/keys-min.txt ] && [ -f out/nsp/logo/NintendoLogo.png ] && [ -f out/nsp/logo/StartupMovie.gif ] || { echo "missing out/nsp/hacbrewpack, keys-min.txt or logo/ (docs/switch/NSP.en.md)"; exit 1; }
DAT=release/nro/switch/nfsu2/nfsu2.dat; [ -f "$DAT" ] || { echo "missing $DAT"; exit 1; }
rm -rf out/nsp/work; mkdir -p out/nsp/work/exefs out/nsp/work/control out/nsp/work/romfs out/nsp/work/logo release/nsp
cp out/nsp/logo/NintendoLogo.png out/nsp/logo/StartupMovie.gif out/nsp/work/logo/
cp "$DAT" out/nsp/work/romfs/nfsu2.dat
D="docker run --rm -v $(pwd -W 2>/dev/null || pwd):/src -w /src devkitpro/devkita64"
MSYS_NO_PATHCONV=1 $D bash runtime/switch/nsp/mk-nso.sh
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src/out/nsp/work devkitpro/devkita64 \
  bash -c '../hacbrewpack -k ../keys-min.txt --titleid 01004e4653550000 2>&1 | tail -4'
cp out/nsp/work/hacbrewpack_nsp/01004e4653550000.nsp "release/nsp/Need for Speed Underground 2 [01004E4653550000][v0].nsp"
ls -la release/nsp
