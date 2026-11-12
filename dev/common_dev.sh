# dev-only helpers (maintainers): patched Supermodel as a test oracle. Users never need this.
. "$ROOT/scripts/common.sh"
makefile_for_os() { if [ "$OS" = Darwin ]; then echo Makefiles/Makefile.OSX; else echo Makefiles/Makefile.UNIX; fi; }

# Runs the patched Supermodel for a while with whatever SM_* env vars the caller exported.
# Headless (Xvfb + xdotool, simulated input) when available on Linux, otherwise interactive: you play.
# usage: run_session SUPERMODEL_DIR ROM_ZIP OUT_PREFIX SECONDS
run_session() {
  local SM=$1 ZIP=$2 OUT=$3 SECS=$4
  if [ "$OS" = Linux ] && [ "${INTERACTIVE:-0}" != 1 ] && command -v xvfb-run >/dev/null && command -v xdotool >/dev/null; then
    echo "   (headless: Xvfb + simulated input, ${SECS}s)"
    xvfb-run -a -s "-screen 0 1280x1024x24" "$ROOT/dev/run_cov.sh" "$SM" "$ZIP" "$OUT" "$SECS" >"$OUT.session.log" 2>&1 || true
  else
    echo ""
    echo "   >>> Supermodel opens now. Play for about ${SECS}s so the code you care about runs:"
    echo "       3 = coin, 1 = start, arrows + Enter in the menus, hold Up to drive. Quit with ESC (this writes the results)."
    echo ""
    ( cd "$SM" && SM_COV="$OUT" bin/supermodel "$ZIP" -no-net ) || true
  fi
}
