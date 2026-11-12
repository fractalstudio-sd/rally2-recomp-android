#!/usr/bin/env bash
# start the game. extra args go to the player (-w N -h N -fullscreen -widescreen -vsync).
# keys: 3 = coin, 1 = start, arrows = steer/gas/brake (see README).
ROOT=$(cd "$(dirname "$0")/.." && pwd); WORK=${WORK:-$ROOT/build}
[ -x "$WORK/bin/srally2" ] || { echo "run scripts/setup.sh and scripts/build.sh first"; exit 1; }
exec "$WORK/bin/srally2" "$WORK/images" "$@"
