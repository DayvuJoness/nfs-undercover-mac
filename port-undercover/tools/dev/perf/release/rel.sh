#!/bin/sh
# rel.sh TAG SECONDS "save overrides k=v,..." "extra keys" [env...]
# A copy of the person's profile (save "ru", their mods) with Extra Options keys on and the given per-save overrides;
# their start keys reach driving at ~40 s; 7 at 45 s turns the autopilot on. Summary in /tmp/rel/TAG.txt.
TAG=$1; DUR=$2; OV=$3; EXTRA=$4; shift 4
S="$HOME/Library/Application Support"; SRC="$S/NFS Undercover REL"
rm -rf "$SRC"; cp -R "$S/NFS Undercover" "$SRC"; rm -rf "$SRC/Logs"
python3 - "$SRC/SaveSettings/${SAVE:-Player1}.json" "hotkeys=1${OV:+,$OV}" <<'PY'
import json,sys
p=sys.argv[1]; d=json.load(open(p))
for kv in sys.argv[2].split(','):
    if not kv: continue
    k,v=kv.split('='); d[k if '/' in k else 'core.nfsu/'+('mod_eo_hotkeys' if k=='hotkeys' else k)]=int(v)
json.dump(d,open(p,'w'),indent=1)
PY
D=/tmp/rel/$TAG; rm -rf $D; mkdir -p $D
START="13:Return:0.1,15.43:Return:0.04,16.69:Return:0.09,17.53:Return:0.13,20:Return:0.1,23:Return:0.1,26:Return:0.1,29:Return:0.1,33:Return:0.1,37:Return:0.1${NOAUTO:-,45:7:0.1}"
PKGSRC="NFS Undercover REL" KS="$START${EXTRA:+,$EXTRA}" sh /tmp/pkgrun2.sh rel$TAG $DUR "" RECOMP_HOST_DUMP_EVERY=${EVERY:-1200} RECOMP_HOST_DUMP_AFTER=${AFTER:-50} RECOMP_HOST_DUMP_DIR=$D "$@" >/dev/null 2>&1
L=/tmp/shot-rel$TAG/latest.log
{
echo "== $TAG overrides [$OV] keys [$EXTRA]"
grep -E "guest exit code|elapsed:|presented frames" $L
grep -E "frame pacing" $L | tail -3
echo "-- faults:"; grep -iE "SIGSEGV|SIGBUS|unhandled|access violation|TerminateProcess|0xc0000|call to unknown target|no block entry|abort|assert|stack overflow|recursion deeper" $L | head -8
echo "-- mods:"; grep -E "core.nfsu: (Autopilot|cops|emergency|police|Police|add-on cars for|heli|career money|extra options key|key ..)|Autopilot|vlt: |hard drivers|Hard Drivers|settings: save" $L | grep -v "dealer row" | head -25
echo "-- shown messages:"; grep -E "core.nfsu: message" $L | head -10
} > /tmp/rel/$TAG.txt 2>&1
ls $D/*.ppm 2>/dev/null | wc -l >> /tmp/rel/$TAG.txt
cat /tmp/rel/$TAG.txt
