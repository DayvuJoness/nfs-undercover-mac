#!/bin/sh
# benchxt.sh TAG AT_S: run bench2 (limit 960) and attach Time Profiler for 12 s at AT_S seconds after start.
TAG=$1; AT=${2:-95}
/tmp/bench2.sh bench-$TAG.log /tmp/ft-$TAG.csv 4 > /tmp/bench-$TAG.out 2>&1 &
sleep $AT
PID=$(pgrep -f "MacOS/UndercoverRecomp" | head -1); echo PID=$PID
rm -rf /tmp/$TAG.trace
xcrun xctrace record --template 'Time Profiler' --attach $PID --time-limit 12s --output /tmp/$TAG.trace > /tmp/$TAG.xtlog 2>&1
xcrun xctrace export --input /tmp/$TAG.trace --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' > /tmp/$TAG.xml 2>/dev/null
wait
cat /tmp/bench-$TAG.out
