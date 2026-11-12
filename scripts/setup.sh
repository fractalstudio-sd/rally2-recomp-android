#!/usr/bin/env bash
# ROM zip -> lifted C. Runs everything locally; nothing generated here is meant to be committed.
# No emulator involved. Needs: python3, JDK 21+ (Ghidra), curl, unzip, a C compiler.
# usage: scripts/setup.sh /path/to/srally2.zip      (env: WORK=build  GHIDRA_DIR=/path/to/ghidra  PROGRAM_SIZE=0x8E000)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$ROOT/scripts/common.sh"; ROM_ZIP=$(abspath "$1")
WORK=${WORK:-$ROOT/build}; mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd); PROGRAM_SIZE=${PROGRAM_SIZE:-0x8E000}
GHIDRA_VER=11.3.2; GHIDRA_URL=https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_11.3.2_build/ghidra_11.3.2_PUBLIC_20250415.zip
need python3 python3; need curl curl; need unzip unzip; need cc "Xcode command line tools / gcc"
# Ghidra 11.3 needs JDK 21..24 (newer JDKs fail with "osgi.ee=UNKNOWN"): pick a suitable one
pick_jdk() {
  for h in "${JAVA_HOME:-}" "$(/usr/libexec/java_home -v 21 2>/dev/null)" "$(brew --prefix openjdk@21 2>/dev/null)/libexec/openjdk.jdk/Contents/Home" /opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home /usr/local/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home /usr/lib/jvm/java-21-openjdk-amd64; do
    [ -n "$h" ] && [ -x "$h/bin/java" ] || continue
    v=$("$h/bin/java" -version 2>&1 | sed -n '1s/[^"]*"\([0-9]*\).*/\1/p')
    [ "${v:-0}" -ge 21 ] && [ "${v:-0}" -le 24 ] && { export JAVA_HOME="$h"; export PATH="$h/bin:$PATH"; return 0; }
  done; return 1; }
pick_jdk || { echo "need JDK 21..24 for Ghidra (macOS: brew install openjdk@21)"; exit 1; }

echo "== 1/4 ROM regions (CRC-checked)"
python3 -I "$ROOT/tools/build_images.py" "$ROOT/data/srally2.json" "$ROM_ZIP" "$WORK/images"
head -c $((PROGRAM_SIZE)) "$WORK/images/crom.bin" > "$WORK/program_at_0x0.bin"
tail -c $((0x100000)) "$WORK/images/crom.bin" > "$WORK/boot_at_0xFFF00000.bin"   # boot block (reset vector 0xFFF00100)

echo "== 2/4 Ghidra headless (static discovery)"
if [ -z "${GHIDRA_DIR:-}" ]; then
  [ -d "$WORK/ghidra_$GHIDRA_VER" ] || { curl -sSL -o "$WORK/ghidra.zip" "$GHIDRA_URL"; mkdir -p "$WORK/ghidra_$GHIDRA_VER"; unzip -q "$WORK/ghidra.zip" -d "$WORK/ghidra_$GHIDRA_VER"; }
  GHIDRA_DIR=$(echo "$WORK"/ghidra_$GHIDRA_VER/ghidra_*)
fi
rm -rf "$WORK/gproj"; mkdir -p "$WORK/gproj"
: > "$WORK/no_seeds.txt"
# seeds: exception vectors (+ reset vector in the boot block)
python3 -I -c "print('\n'.join('%x' % (0x100*i) for i in range(1,21)))" > "$WORK/seeds_prog.txt"
python3 -I -c "print('\n'.join('%x' % (0xFFF00000+0x100*i) for i in range(1,21)))" > "$WORK/seeds_boot.txt"
python3 -I "$ROOT/tools/find_code_ptrs.py" "$WORK/program_at_0x0.bin" "$WORK/seeds_ptr.txt" "$WORK/images/crom.bin"
cat "$WORK/seeds_ptr.txt" >> "$WORK/seeds_prog.txt"
ghidra_one() {  # name image base seedfile
  "$GHIDRA_DIR/support/analyzeHeadless" "$WORK/gproj" "$1" -import "$2" -processor "PowerPC:BE:32:default" -loader BinaryLoader -loader-baseAddr "$3" \
    -scriptPath "$ROOT/ghidra" -postScript SeedAndReport.java "$4" "$WORK/no_seeds.txt" /dev/null >"$WORK/ghidra_$1.log" 2>&1
  grep -E "FUNCTIONS_TOTAL|INSTRUCTIONS" "$WORK/ghidra_$1.log" || { echo "Ghidra failed, see $WORK/ghidra_$1.log"; exit 1; }
  "$GHIDRA_DIR/support/analyzeHeadless" "$WORK/gproj" "$1" -process "$(basename "$2")" -noanalysis -scriptPath "$ROOT/ghidra" -postScript ExportFunctions.java "$WORK/functions_$1.json" >>"$WORK/ghidra_$1.log" 2>&1
}
ghidra_one prog "$WORK/program_at_0x0.bin" 0x0 "$WORK/seeds_prog.txt"
ghidra_one boot "$WORK/boot_at_0xFFF00000.bin" 0xFFF00000 "$WORK/seeds_boot.txt"

echo "== 3/4 lift to C"
mkdir -p "$WORK/out"; rm -f "$WORK"/out/recomp_funcs*.c; cp "$ROOT/runtime/ppc_rt.h" "$WORK/out/"
: > "$WORK/no_edges.txt"
python3 -I "$ROOT/lifter/gen_funcs.py" "$WORK/out" "$WORK/no_edges.txt" "$WORK/images/crom.bin" \
  "$WORK/program_at_0x0.bin:0x0:$WORK/functions_prog.json" "$WORK/boot_at_0xFFF00000.bin:0xFFF00000:$WORK/functions_boot.json"

python3 -I "$ROOT/tools/apply_overrides.py" "$WORK/out"   # route the functions the runtime replaces (touch menus)

echo "== 4/4 compile check of the generated C"
for f in "$WORK"/out/recomp_funcs_*.c; do cc -O0 -w -fsyntax-only -I"$WORK/out" "$f" || exit 1; done && cc -O0 -w -fsyntax-only -I"$WORK/out" "$WORK/out/recomp_dispatch.c" && echo "generated C compiles"
echo "done: lifted C in $WORK/out  (next: scripts/build.sh)"
