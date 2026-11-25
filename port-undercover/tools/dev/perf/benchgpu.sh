#!/bin/sh
# benchgpu.sh TAG AT_S: bench (person's settings, 120 synced) + Metal System Trace for 4 s at AT_S.
TAG=$1; AT=${2:-140}
BENCH0=BenchP /tmp/bench2.sh bench-$TAG.log /tmp/ft-$TAG.csv 3 > /tmp/bench-$TAG.out 2>&1 &
sleep $AT
PID=$(pgrep -f "MacOS/UndercoverRecomp" | head -1)
rm -rf /tmp/$TAG.trace
xcrun xctrace record --template 'Metal System Trace' --attach $PID --time-limit 4s --output /tmp/$TAG.trace > /tmp/$TAG.xtlog 2>&1
wait
