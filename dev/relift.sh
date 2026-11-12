#!/usr/bin/env bash
# maintainers: re-run only the lifting step on an existing build dir (after lifter changes).  usage: dev/relift.sh WORKDIR
set -euo pipefail
W=$(cd "$1" && pwd); ROOT=$(cd "$(dirname "$0")/.." && pwd)
rm -f "$W"/out/recomp_funcs*.c; cp "$ROOT/runtime/ppc_rt.h" "$W/out/"
python3 -I "$ROOT/lifter/gen_funcs.py" "$W/out" "$W/no_edges.txt" "$W/images/crom.bin" \
  "$W/program_at_0x0.bin:0x0:$W/functions_prog.json" "$W/boot_at_0xFFF00000.bin:0xFFF00000:$W/functions_boot.json"
