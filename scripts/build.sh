#!/usr/bin/env bash
# lifted C (from setup.sh) -> native game binary build/bin/srally2.   Linux + macOS.
# needs: cmake, SDL2 (macOS: brew install cmake sdl2), a C/C++ compiler.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$ROOT/scripts/common.sh"
WORK=${WORK:-$ROOT/build}
need cmake "macOS: brew install cmake"
[ -f "$WORK/out/recomp_dispatch.c" ] || { echo "run scripts/setup.sh /path/to/srally2.zip first"; exit 1; }
EXTRA=()
if [ "$OS" = Darwin ] && command -v brew >/dev/null 2>&1; then EXTRA+=(-DCMAKE_PREFIX_PATH="$(brew --prefix)"); fi
# M3_NONFUSED=1: separate multiply+add like the reference emulator instead of fused (debug / A-B tests)
if [ "${M3_NONFUSED:-0}" = 1 ]; then EXTRA+=(-DCMAKE_C_FLAGS=-DPPC_NONFUSED_FMA -DCMAKE_CXX_FLAGS=-DPPC_NONFUSED_FMA); WORK_CMAKE="$WORK/cmake_nf"; else WORK_CMAKE="$WORK/cmake"; fi
cmake -S "$ROOT" -B "$WORK_CMAKE" -DCMAKE_BUILD_TYPE=Release -DM3_GENERATED_DIR="$WORK/out" ${EXTRA[@]+"${EXTRA[@]}"}
cmake --build "$WORK_CMAKE" -j "$(ncpu)"
mkdir -p "$WORK/bin"; cp "$WORK_CMAKE/srally2" "$WORK/bin/srally2"
echo "built $WORK/bin/srally2  (run: scripts/run.sh)"
