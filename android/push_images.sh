#!/usr/bin/env bash
# Copy the game images (built by scripts/setup.sh from YOUR ROMs) to the phone. USB debugging must be on.
#   android/push_images.sh [images_dir] [extra game options, e.g. "-ires 720"]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$ROOT/build/images}"
ADB="${ADB:-adb}"; command -v "$ADB" >/dev/null || ADB="${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb"
[ -f "$IMG/crom.bin" ] || { echo "no images in $IMG - run scripts/setup.sh first" >&2; exit 1; }
DST=/sdcard/Android/data/org.srally2/files
"$ADB" shell mkdir -p "$DST/images"
echo "copying $(du -sh "$IMG" | cut -f1) ..."
"$ADB" push "$IMG/." "$DST/images/"
[ -n "${2:-}" ] && { "$ADB" shell "echo '$2' > $DST/args.txt"; echo "options: $2"; }
echo "done - start 'Sega Rally 2' on the phone."
