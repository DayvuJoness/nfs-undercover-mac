#!/bin/sh
# bench2.sh LOG CSV LIMIT [env...]: Bench0 copy with frame_limit LIMIT (4 = maximum 960), the same 200 s drive.
pgrep -lf "MacOS/UndercoverRecomp" && exit 1
cd "$HOME/Documents/Need for Speed - Undercover/port-source-carbon/build"
P="$HOME/Library/Application Support/NFS Undercover Bench"
rm -rf "$P"; cp -R "$HOME/Library/Application Support/NFS Undercover ${BENCH0:-Bench0}" "$P"
LOG=$1; CSV=$2; LIM=$3; shift 3
python3 - "$P/mod-settings.json" $LIM <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p)); d["host.display/frame_limit"]=int(sys.argv[2]); json.dump(d,open(p,'w'),indent=1)
PY
env "$@" RECOMP_FRAME_TIMINGS=$CSV NFSU_PROFILE="NFS Undercover Bench" RECOMP_GEOID=203 RECOMP_FREE_THREADS="${FT-RWAudioCore Dac,RenderThread}" RECOMP_KEY_SCRIPT="32:Return,45:Return,60:Return,75:Return,90:Return,110:Return,125:Up:20,145:Down:2,147:Left:1,149:Up:20,170:Down:2,172:Right:1,174:Up:25" sh run.sh 200 $LOG 1 >/dev/null 2>&1
grep "frame pacing" $LOG | tail -2
python3 /tmp/fps.py $CSV
