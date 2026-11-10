#!/bin/bash
# Plays the KEX demo scenes (the README's GIFs) in a KEX Notepad++ under Wine + Xvfb and turns each
# recording into a GIF. Wine draws its own window frame and controls, so this is for trying the scenes;
# the README's GIFs are recorded on Windows with record-gifs.ps1.
#
#   suite/test/run-demo.sh NPP_DIR OUT_DIR [scenes]
#
# NPP_DIR  portable KEX Notepad++ (its kex.ini and session are reset)
# OUT_DIR  where the .kexrec recordings, the GIFs and the log go
# scenes   "all" (default) or a comma-separated list: motion,tabs,glint,splash,swipe,settings
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NPP=$1
OUT=$2
SCENES=${3:-all}
export WINEDEBUG=-all WINEPREFIX=$HOME/.wine-kexsuite DISPLAY=:87
TESTDIR=$WINEPREFIX/drive_c/kexdemo
WTESTDIR='C:\kexdemo'
LOG=$TESTDIR/selftest-log.txt

mkdir -p "$OUT"
rm -f "$NPP/kex.ini" "$NPP/kex.log" "$NPP/session.xml"
rm -rf "$TESTDIR"; mkdir -p "$TESTDIR"

Xvfb :87 -screen 0 1600x1000x24 >/dev/null 2>&1 & XPID=$!
sleep 2
NPPWIN="Z:$(echo "$NPP" | sed 's#/#\\#g')\\notepad++.exe"
(cd "$NPP" && wine explorer /desktop=kexd,1600x1000 "$NPPWIN" -nosession \
  "-pluginMessage=KexSuite_selftest=1;KexSuite_demo=$SCENES;KexSuite_testdir=$WTESTDIR" >/dev/null 2>&1) &

for i in $(seq 1 3000); do
  sleep 0.2
  [ -f "$LOG" ] && grep -aq '^DONE' "$LOG" && break
done
sleep 1
wineserver -k 2>/dev/null
kill $XPID 2>/dev/null
cp "$LOG" "$OUT/demo-log.txt" 2>/dev/null
grep -a '^DEMO' "$LOG"
for rec in "$TESTDIR"/*.kexrec; do
  [ -f "$rec" ] || continue
  name=$(basename "$rec" .kexrec)
  cp "$rec" "$OUT/"
  python3 "$HERE/kexrec2gif.py" "$rec" "$OUT/$name.gif"
done
