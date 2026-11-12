#!/usr/bin/env bash
# Lockstep verification of the lifted instructions against Supermodel (registers, flags, FPRs, every bus access).
# usage: dev/verify.sh /path/to/srally2.zip      (run dev/build_oracle.sh and scripts/setup.sh first; env: WORK, TRACE_SECONDS=240)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$ROOT/dev/common_dev.sh"; ROM_ZIP=$(abspath "$1"); WORK=${WORK:-$ROOT/build}; PROGRAM_SIZE=${PROGRAM_SIZE:-0x8E000}
mkdir -p "$WORK/verify"; cd "$WORK/verify"
echo "== windowed trace (10k records every 100M instructions)"
rm -f trace.bin trace.bin.mem
export SM_TRACE="$WORK/verify/trace.bin" SM_TRACE_N=10000 SM_TRACE_PERIOD=100000000 SM_TRACE_SKIP=20000000
run_session "$WORK/supermodel" "$ROM_ZIP" "$WORK/verify/cov" "${TRACE_SECONDS:-240}"
unset SM_TRACE SM_TRACE_N SM_TRACE_PERIOD SM_TRACE_SKIP
[ -s trace.bin ] || { echo "no trace produced (quit Supermodel with ESC)"; exit 1; }
echo "== step table (one lifted instruction per case) + harness"
cp "$ROOT/runtime/ppc_rt.h" "$ROOT/dev/verify/verify.c" .
python3 -I "$ROOT/lifter/lifter.py" "$WORK/program_at_0x0.bin" lifted_step.c $PROGRAM_SIZE
gcc -O0 -w -DPPC_NONFUSED_FMA -c lifted_step.c -o lifted_step.o        # NONFUSED: Supermodel computes fmadd in two steps
gcc -O1 -w -DPPC_NONFUSED_FMA -o verify verify.c lifted_step.o -lm
./verify trace.bin "$WORK/program_at_0x0.bin" 100000000 10000
