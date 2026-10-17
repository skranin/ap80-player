#!/bin/sh
# Usage: tools/shot.sh name  -> screenshots/name.png (from the running ap80player)
D=$(cd "$(dirname "$0")/.." && pwd); mkdir -p "$D/screenshots"
adb shell "pidof ap80player >/dev/null || { echo not running; exit 1; }; echo shot /tmp/shot.ppm > /tmp/ap80p.cmd; sleep 1" && adb pull /tmp/shot.ppm "$D/screenshots/$1.ppm" >/dev/null && python3 -I "$D/tools/ppm2png.py" "$D/screenshots/$1.ppm" "$D/screenshots/$1.png" && rm "$D/screenshots/$1.ppm" && echo "$D/screenshots/$1.png"
