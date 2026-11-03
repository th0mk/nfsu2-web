# Runs inside the devkitpro/devkita64 container (cwd = repo root): the ELF -> NSO, the NPDM, the NACP and the icon, into out/nsp/work.
set -e
cd /src
T=/opt/devkitpro/tools/bin
$T/elf2nso out/switch/nfsu2.elf out/nsp/work/exefs/main
$T/npdmtool runtime/switch/nsp/app.json out/nsp/work/exefs/main.npdm
$T/nacptool --create "Need for Speed: Underground 2" "Electronic Arts" "b$(cat runtime/switch/BUILD)" out/nsp/work/control/control.nacp
cp runtime/switch/icon.jpg out/nsp/work/control/icon_AmericanEnglish.dat
