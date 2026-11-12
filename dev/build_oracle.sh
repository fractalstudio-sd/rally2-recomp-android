#!/usr/bin/env bash
# MAINTAINERS ONLY: builds Supermodel (pinned) + trace patch, used as a test oracle for the lifter. Not part of the user flow.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$ROOT/dev/common_dev.sh"
WORK=${WORK:-$ROOT/build}; mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SM_COMMIT=8b4de239bf3e0fd921f4383edcd3c21c2e62067b
rm -rf "$WORK/supermodel"; git init -q "$WORK/supermodel"; git -C "$WORK/supermodel" remote add origin https://github.com/trzy/Supermodel
git -C "$WORK/supermodel" fetch -q --depth 1 origin "$SM_COMMIT"; git -C "$WORK/supermodel" checkout -q FETCH_HEAD
git -C "$WORK/supermodel" apply --ignore-whitespace "$ROOT/dev/supermodel/trace-coverage.patch"
make -C "$WORK/supermodel" -f "$(makefile_for_os)" -j"$(ncpu)" NET_BOARD=0
echo "oracle built: $WORK/supermodel/bin/supermodel"
