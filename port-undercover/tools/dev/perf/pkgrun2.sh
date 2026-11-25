#!/bin/sh
# pkgrun.sh TAG SECONDS "shot times" [env...]: the packaged dist app on a copy of the person's profile, silent,
# screen captures (what the person sees) at the given process ages; Chrome back in front after.
TAG=$1; DUR=$2; SHOTS=$3; shift 3
APP="${APP:-$HOME/Documents/Need for Speed - Undercover/port-source-carbon/dist/Need for Speed Undercover.app}"
until [ "$(ioreg -c IOHIDSystem | awk '/HIDIdleTime/ {print int($NF/1000000000); exit}')" -ge 30 ] && ! pgrep -q -f "MacOS/Undercover[R]ecomp"; do sleep 5; done
P="$HOME/Library/Application Support/NFS Undercover PkgRun"
rm -rf "$P"; cp -R "$HOME/Library/Application Support/${PKGSRC:-NFS Undercover PQ}" "$P"; rm -rf "$P/Logs"; cp "$HOME/Library/Application Support/NFS Undercover/mod-settings.json" "$P/"
[ -n "$NOWARM" ] && rm -rf "$P/ShaderWarm"
[ -n "$MS" ] && python3 - "$P/mod-settings.json" "$MS" <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p))
for kv in sys.argv[2].split(","):
    k,v=kv.split("="); d[k if "/" in k else "core.nfsu/"+k]=int(v)
json.dump(d,open(p,"w"),indent=1)
PY
rm -rf /tmp/shot-$TAG; mkdir -p /tmp/shot-$TAG
caffeinate -d -t $((DUR+20)) &
( env RECOMP_PROFILE_DIR="$P" SDL_AUDIO_DRIVER=dummy RECOMP_KEY_SCRIPT="${KS:-32:Return,45:Return,60:Return,75:Up:6}" "$@" \
    "$APP/Contents/MacOS/Launch" >/tmp/shot-$TAG/out.log 2>&1 & )
t0=$(date +%s)
for s in $SHOTS; do while [ $(( $(date +%s) - t0 )) -lt $s ]; do sleep 0.5; done; screencapture -x /tmp/shot-$TAG/s$s.png; done
while [ $(( $(date +%s) - t0 )) -lt $DUR ]; do sleep 1; done
pkill -f "MacOS/Undercover[R]ecomp"; sleep 3; pkill -9 -f "MacOS/Undercover[R]ecomp"; sleep 1
cp "$P"/Logs/*.log /tmp/shot-$TAG/ 2>/dev/null
ls /tmp/shot-$TAG
pgrep -q -f "MacOS/Undercover[R]ecomp" || open -a "Google Chrome"
