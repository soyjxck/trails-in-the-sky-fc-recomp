#!/bin/sh
# Build the portable host: the lifted game on pcrecomp's win32hle, as the
# machine's own 64-bit program (Linux), or for the Switch with devkitA64.
#
#   tools/build_portable.sh [linux|switch] [build folder]
#
# The lifted C (src/recomp/gen) is made on Windows by run_lift.py; it is the
# same C on every host. Objects are kept, so a second run compiles only what
# changed. JOBS sets the number of compilers (default: every core).
#
# The game's music is Ogg Vorbis from encoders old enough to need the real
# libvorbis (floor 0). The Switch build takes devkitPro's (switch-libvorbis).
# The Linux build takes the system's if pkg-config knows it, or the one named
# by VORBIS_CFLAGS and VORBIS_LIBS; with neither, a built-in decoder is used
# that cannot play most of the tracks.
#
# The movies are played with ffmpeg (src/portable/movie.c): FFMPEG_CFLAGS and
# FFMPEG_LIBS say where it is; on Linux pkg-config is asked if they are not
# given. Without it the movies are skipped. devkitPro's is switch-ffmpeg.
set -eu
TARGET=${1:-linux}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${2:-$ROOT/build-$TARGET}
GEN=${GEN:-$ROOT/src/recomp/gen}
HLE=$ROOT/pcrecomp/runtime/win32hle
JOBS=${JOBS:-$(nproc)}
OPT=${OPT:--O1}
HOST_OPT=${HOST_OPT:--O1}
mkdir -p "$OUT/obj"

case "$TARGET" in
linux)
  CC=${CC:-gcc}
  CFLAGS="$(pkg-config --cflags sdl2 SDL2_ttf)"
  LIBS="-rdynamic $(pkg-config --libs sdl2 SDL2_ttf) -lGLESv2 -lEGL -lpthread"
  if [ -z "${VORBIS_LIBS:-}" ] && pkg-config --exists vorbisfile 2>/dev/null; then
    VORBIS_CFLAGS=$(pkg-config --cflags vorbisfile)
    VORBIS_LIBS=$(pkg-config --libs vorbisfile)
  fi
  if [ -n "${VORBIS_LIBS:-}" ]; then
    CFLAGS="$CFLAGS -DHLE_WITH_LIBVORBIS ${VORBIS_CFLAGS:-}"
    LIBS="$LIBS $VORBIS_LIBS"
  else
    echo "no libvorbis: most of the game's music will not play (see the top of this script)" >&2
  fi
  if [ -z "${FFMPEG_LIBS:-}" ] && pkg-config --exists libavformat libavcodec libswscale libswresample libavutil 2>/dev/null; then
    FFMPEG_CFLAGS=$(pkg-config --cflags libavformat libavcodec libswscale libswresample libavutil)
    FFMPEG_LIBS=$(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil)
  fi
  if [ -n "${FFMPEG_LIBS:-}" ]; then
    CFLAGS="$CFLAGS -DFC_WITH_FFMPEG ${FFMPEG_CFLAGS:-}"
    LIBS="$LIBS $FFMPEG_LIBS"
  else
    echo "no ffmpeg: the movies will be skipped (see the top of this script)" >&2
  fi
  LIBS="$LIBS -lm"
  EXE=$OUT/trails-portable
  ;;
switch)
  : "${DEVKITPRO:=/opt/devkitpro}"
  CC=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc
  # Frame pointers are kept: the profiler (--profile) follows them to say who
  # called what it found the game in.
  ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -fno-omit-frame-pointer"
  PKG=$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config
  CFLAGS="$ARCH -D__SWITCH__ -DHLE_WITH_LIBVORBIS -ffunction-sections -I$DEVKITPRO/libnx/include -I$DEVKITPRO/portlibs/switch/include $($PKG --cflags sdl2 SDL2_ttf)"
  LIBS="-specs=$DEVKITPRO/libnx/switch.specs $ARCH -L$DEVKITPRO/libnx/lib $($PKG --libs --static sdl2 SDL2_ttf) -lEGL -lGLESv2 -lglapi -ldrm_nouveau -L$DEVKITPRO/portlibs/switch/lib -lvorbisfile -lvorbis -logg -lnx -lm"
  if [ -z "${FFMPEG_LIBS:-}" ] && [ -f "$DEVKITPRO/portlibs/switch/lib/libavformat.a" ]; then
    FFMPEG_CFLAGS="-I$DEVKITPRO/portlibs/switch/include"
    FFMPEG_LIBS="-L$DEVKITPRO/portlibs/switch/lib -lavformat -lavcodec -lswscale -lswresample -lavutil -ldav1d -lbz2 -lz"
  fi
  if [ -n "${FFMPEG_LIBS:-}" ]; then
    CFLAGS="$CFLAGS -DFC_WITH_FFMPEG ${FFMPEG_CFLAGS:-}"
    LIBS=$(echo "$LIBS" | sed "s| -lnx -lm\$| $FFMPEG_LIBS -lnx -lm|")
  else
    echo "no ffmpeg: the movies will be skipped (see the top of this script)" >&2
  fi
  EXE=$OUT/trails.elf
  ;;
*) echo "unknown target $TARGET" >&2; exit 1 ;;
esac

INC="-I$ROOT/pcrecomp/runtime/recomp32 -I$HLE -I$GEN -I$ROOT/src/portable -I$ROOT/src/mods"
DEFS="-DRECOMP_RETADDR=0u -DRECOMP_LOCAL_REGS -D_GNU_SOURCE -DHLE_WITH_D3D9"

# One line per source: "<object> <source> <flags>".
LIST=$OUT/sources.txt
: > "$LIST"
for f in "$GEN"/*.c; do
  echo "$OUT/obj/gen_$(basename "$f" .c).o $f -w $OPT" >> "$LIST"
done
for f in "$HLE"/*.c; do
  case "$f" in *_selftest.c) continue ;; esac
  echo "$OUT/obj/hle_$(basename "$f" .c).o $f $HOST_OPT -Wall -Wno-unused-function -Wno-unused-but-set-variable" >> "$LIST"
done
# guest C++ exceptions: the one file of native32 that is not Windows' own
echo "$OUT/obj/eh32.o $ROOT/pcrecomp/runtime/native32/eh32.c $HOST_OPT -Wall -Wno-unused-function" >> "$LIST"
for f in "$ROOT"/src/portable/*.c "$ROOT"/src/mods/*.c "$ROOT"/src/common/*.c; do
  echo "$OUT/obj/host_$(basename "$f" .c).o $f $HOST_OPT -Wall" >> "$LIST"
done

# A source is compiled again when it, or a header it may include, is newer
# than its object: the lifted C includes only the lifter's headers and its
# own, everything else may include any of them. (A header that changes the
# shape of something shared, compiled into half the objects, is a fault found
# days later.) Other flags are not noticed: a build with another OPT wants a
# folder of its own.
DEPS_GEN="$ROOT/pcrecomp/runtime/recomp32 $GEN"
DEPS_HOST="$DEPS_GEN $HLE $ROOT/pcrecomp/runtime/native32 $ROOT/src/portable $ROOT/src/mods"
export CC CFLAGS INC DEFS DEPS_GEN DEPS_HOST

# xargs runs the compilers in parallel; sh -c gets one source line each.
< "$LIST" xargs -P "$JOBS" -L 1 sh -c '
  obj=$0; src=$1; shift
  case "$(basename "$obj")" in gen_*) deps=$DEPS_GEN ;; *) deps=$DEPS_HOST ;; esac
  # shellcheck disable=SC2086
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ -n "$(find $deps -maxdepth 1 -name "*.h" -newer "$obj" -print -quit)" ]; then
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
  # The check program: started the same way, it says what the game needs of
  # the console and the card, and whether it is there.
  # shellcheck disable=SC2086
  $CC $ARCH -D__SWITCH__ -O1 -Wall -I$DEVKITPRO/libnx/include -I$DEVKITPRO/portlibs/switch/include "$ROOT/tools/switch_check.c" \
    -specs=$DEVKITPRO/libnx/switch.specs -L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib \
    -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lstdc++ -lnx -lm -o "$OUT/trails-check.elf"
  "$DEVKITPRO/tools/bin/nacptool" --create "Trails in the Sky FC: check" "recomp" "0.1.0" "$OUT/trails-check.nacp"
  "$DEVKITPRO/tools/bin/elf2nro" "$OUT/trails-check.elf" "$OUT/trails-check.nro" --nacp="$OUT/trails-check.nacp" --icon="$DEVKITPRO/libnx/default_icon.jpg"
  ls -la "$OUT/trails-check.nro"
else
  ls -la "$EXE"
fi
