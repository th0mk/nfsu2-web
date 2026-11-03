#!/bin/bash
# The OpenGL probe for the Switch (gltest.c) -> out/switch/gltest.nro.  Runs itself in the devkitpro/devkita64 image.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
if [ ! -d /opt/devkitpro/devkitA64 ]; then
  MSYS_NO_PATHCONV=1 exec docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w //src devkitpro/devkita64 bash runtime/switch/build-gltest.sh "$@"
fi
export PATH=/opt/devkitpro/devkitA64/bin:$PATH
mkdir -p out/switch
aarch64-none-elf-gcc -O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -specs=/opt/devkitpro/libnx/switch.specs -D__SWITCH__ \
  -I/opt/devkitpro/libnx/include -I/opt/devkitpro/portlibs/switch/include -o out/switch/gltest.elf runtime/switch/gltest.c \
  -L/opt/devkitpro/libnx/lib -L/opt/devkitpro/portlibs/switch/lib -lglad -lEGL -lglapi -ldrm_nouveau -lnx -lstdc++ -lm
/opt/devkitpro/tools/bin/nacptool --create "NFSU2 GL test" "nfsu2-web" "0.1" out/switch/gltest.nacp
/opt/devkitpro/tools/bin/elf2nro out/switch/gltest.elf out/switch/gltest.nro --nacp=out/switch/gltest.nacp --icon=/opt/devkitpro/libnx/default_icon.jpg
ls -l out/switch/gltest.nro
