# shared helpers for setup.sh / verify.sh (Linux + macOS)
OS=$(uname -s)
abspath() { python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$1"; }
ncpu() { nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1  ($2)"; exit 1; }; }
