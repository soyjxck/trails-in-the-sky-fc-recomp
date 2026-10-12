#!/bin/sh
# Build the portable host: the lifted game on pcrecomp's win32hle, as the
# machine's own 64-bit program (Linux), or for the Switch with devkitA64.
#
#   tools/build_portable.sh [linux|switch] [build folder]
#
# The lifted C (src/recomp/gen) is made on Windows by run_lift.py; it is the
# same C on every host. Objects are kept, so a second run compiles only what
# changed. JOBS sets the number of compilers (default: every core).
set -eu
TARGET=${1:-linux}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${2:-$ROOT/build-$TARGET}
GEN=${GEN:-$ROOT/src/recomp/gen}
HLE=$ROOT/pcrecomp/runtime/win32hle
JOBS=${JOBS:-$(nproc)}
OPT=${OPT:--O1}
mkdir -p "$OUT/obj"

case "$TARGET" in
linux)
  CC=${CC:-gcc}
  CFLAGS="$(pkg-config --cflags sdl2 SDL2_ttf)"
  LIBS="-rdynamic $(pkg-config --libs sdl2 SDL2_ttf) -lGLESv2 -lEGL -lpthread -lm"
  EXE=$OUT/trails-portable
  ;;
switch)
  : "${DEVKITPRO:=/opt/devkitpro}"
  CC=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc
  ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
  PKG=$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config
  CFLAGS="$ARCH -D__SWITCH__ -ffunction-sections -I$DEVKITPRO/libnx/include $($PKG --cflags sdl2 SDL2_ttf)"
  LIBS="-specs=$DEVKITPRO/libnx/switch.specs $ARCH -L$DEVKITPRO/libnx/lib $($PKG --libs --static sdl2 SDL2_ttf) -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lnx -lm"
  EXE=$OUT/trails.elf
  ;;
*) echo "unknown target $TARGET" >&2; exit 1 ;;
esac

INC="-I$ROOT/pcrecomp/runtime/recomp32 -I$HLE -I$GEN -I$ROOT/src/portable"
DEFS="-DRECOMP_RETADDR=0u -DRECOMP_LOCAL_REGS -D_GNU_SOURCE -DHLE_WITH_D3D9"

# One line per source: "<object> <source> <flags>".
LIST=$OUT/sources.txt
: > "$LIST"
for f in "$GEN"/*.c; do
  echo "$OUT/obj/gen_$(basename "$f" .c).o $f -w $OPT" >> "$LIST"
done
for f in "$HLE"/*.c; do
  case "$f" in *_selftest.c) continue ;; esac
  echo "$OUT/obj/hle_$(basename "$f" .c).o $f -O1 -Wall -Wno-unused-function -Wno-unused-but-set-variable" >> "$LIST"
done
# guest C++ exceptions: the one file of native32 that is not Windows' own
echo "$OUT/obj/eh32.o $ROOT/pcrecomp/runtime/native32/eh32.c -O1 -Wall -Wno-unused-function" >> "$LIST"
for f in "$ROOT"/src/portable/*.c; do
  echo "$OUT/obj/host_$(basename "$f" .c).o $f -O1 -Wall" >> "$LIST"
done

export CC CFLAGS INC DEFS

# xargs runs the compilers in parallel; sh -c gets one source line each.
< "$LIST" xargs -P "$JOBS" -L 1 sh -c '
  obj=$0; src=$1; shift
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    $CC $CFLAGS $INC $DEFS "$@" -c "$src" -o "$obj" || { echo "FAILED $src" >&2; exit 255; }
    echo "  $(basename "$src")"
  fi'

# shellcheck disable=SC2086
$CC -o "$EXE" "$OUT"/obj/*.o $LIBS
if [ "$TARGET" = switch ]; then
  # An NRO without the asset section (name, icon) is one an emulator's loader
  # may refuse; the icon is libnx's until the port has its own.
  "$DEVKITPRO/tools/bin/nacptool" --create "Trails in the Sky FC" "recomp" "0.1.0" "$OUT/trails.nacp"
  "$DEVKITPRO/tools/bin/elf2nro" "$EXE" "$OUT/trails.nro" --nacp="$OUT/trails.nacp" --icon="$DEVKITPRO/libnx/default_icon.jpg"
  ls -la "$OUT/trails.nro"
else
  ls -la "$EXE"
fi
