#!/bin/sh
# Unattended run of the portable host on Linux: no window, a fresh copy of
# run/saves_base as the game's Saved Games folder, the picture saved as PNGs
# in run/portable/shots, the log in run/portable/trails.log.
#
#   tools/run_portable.sh [frames] [a shot every N frames] [build folder]
#
# GAME is the game's folder (default: Trails in the Sky FC in the checkout).
# LIMIT is the most seconds the run may take (default 300), TAIL how much of
# the log to show, ARGS more options for the host. SOUND=1 switches music and
# sound effects on in the run's settings; nothing is heard either way (SDL's
# dummy audio driver takes the samples). MOVIES=1 switches the logo and
# opening movies on.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FRAMES=${1:-600}
EVERY=${2:-100}
OUT=${3:-$ROOT/build-linux}
RUN=${RUN_DIR:-$ROOT/run/portable}
rm -rf "$RUN/saves" "$RUN/shots"
mkdir -p "$RUN/shots"
if [ -d "$ROOT/run/saves_base" ]; then cp -r "$ROOT/run/saves_base" "$RUN/saves"; else mkdir -p "$RUN/saves"; fi
if [ -n "${SOUND:-}" ] && [ -f "$RUN/saves/FALCOM/ED6/config.ini" ]; then
  sed -i 's/^\(BgmOff\|SeOff\)=1/\1=0/' "$RUN/saves/FALCOM/ED6/config.ini"
fi
if [ -n "${MOVIES:-}" ] && [ -f "$RUN/saves/FALCOM/ED6/config.ini" ]; then
  sed -i 's/^LogoPlayOff=1/LogoPlayOff=0/; s/^OpeningPlayOff=1/OpeningPlayOff=0/' "$RUN/saves/FALCOM/ED6/config.ini"
fi
cd "${GAME:-$ROOT/Trails in the Sky FC}"
code=0
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}      # a test run is never heard
# shellcheck disable=SC2086
timeout "${LIMIT:-300}" "$OUT/trails-portable" --headless --frames "$FRAMES" --shots "$RUN/shots" \
  --shot-every "$EVERY" --saves "$RUN/saves" ${ARGS:-} ed6_win_DX9.exe > "$RUN/trails.log" 2>&1 || code=$?
tail -n "${TAIL:-30}" "$RUN/trails.log"
echo "exit code $code; $(ls "$RUN/shots" | wc -l) pictures in $RUN/shots"
