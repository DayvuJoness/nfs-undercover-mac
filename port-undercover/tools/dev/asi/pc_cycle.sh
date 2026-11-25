#!/bin/sh
# rebuild the adapted plugin into the game and run a test copy with Police Career on; print its log
cd "$HOME/Documents/Need for Speed - Undercover/port-source-carbon"
JH=$HOME/Library/NFSC-Native-Setup/jdk/jdk-26.0.1.jdk/Contents/Home
.venv/bin/python tools/analyze.py --ghidra-home $HOME/Library/NFSC-Native-Setup/ghidra/ghidra_12.1.3_PUBLIC --java-home "$JH" --only policecareer.asi >/tmp/pcana.log 2>&1 || { tail -5 /tmp/pcana.log; exit 1; }
.venv/bin/python tools/build.py --regenerate > /tmp/pcbuild.log 2>&1
grep -E "FAILED|unhandled|dispatches to|error:|Build failed" /tmp/pcbuild.log | grep -v FFmpeg | head -5
grep -q "^built " /tmp/pcbuild.log || exit 1
rm -rf /tmp/pctest
.venv/bin/python tools/setup_kit/package.py --game "../new_nfsu/Need For Speed - Undercover" --out /tmp/pctest >/dev/null 2>&1
I="/tmp/pctest/Need for Speed Undercover.app/Contents/Resources/Game/SCRIPTS/PoliceCareer.ini"
sed -i '' 's/^DebugLog=0/DebugLog=1/' "$I"
APP="/tmp/pctest/Need for Speed Undercover.app" KS="${KS:-32:Return,45:Return,60:Return,75:Up:6}" sh /tmp/pkgrun2.sh ${TAG:-pcx} ${DUR:-150} "${SHOTS:-}" NFSU_POLICE_CAREER=1 $EXTRA >/dev/null 2>&1
L="$HOME/Library/Application Support/NFS Undercover PkgRun/Logs/PoliceCareer.log"
grep -v PERF "$L" | cut -c26-230 | head -${LINES:-60}
grep -h "code patch\|not a translated\|call to unknown\|FATAL\|fault at" /tmp/shot-${TAG:-pcx}/latest.log | head
grep -h "presented frames" /tmp/shot-${TAG:-pcx}/latest.log
