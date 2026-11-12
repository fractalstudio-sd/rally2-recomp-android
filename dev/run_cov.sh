#!/bin/bash
# usage: run_cov.sh SUPERMODEL_DIR ZIP OUTPREFIX SECONDS
SM=$1; ZIP=$2; OUT=$3; SECS=$4
cd "$SM"
export SM_COV="$OUT" SDL_AUDIODRIVER=dummy   # set SM_DUMP="20,100,..." (millions of instructions) to also dump RAM
bin/supermodel "$ZIP" -no-sound -no-throttle -res=496,384 -no-net &
PID=$!
sleep 20; xdotool key 3; sleep 1                       # coin
t=0
while kill -0 $PID 2>/dev/null && [ $t -lt $SECS ]; do
  xdotool key 1; sleep 3; xdotool key Return 2>/dev/null
  [ $t -gt 90 ] && xdotool keydown Up   # gas
  t=$((t+4))
done
kill -INT $PID
for i in $(seq 90); do kill -0 $PID 2>/dev/null || break; sleep 1; done   # let the emulator finish writing its dumps before X goes away
