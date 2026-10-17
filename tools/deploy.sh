#!/bin/sh
# Build ap80player, push it (and boot.sh) to the SD card, and restart it on the device.
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
(cd "$D/app/player" && docker run --rm -v "$PWD":/p -w /p ap80-tc make 2>&1 | grep -E "error" && exit 1 || true)
adb shell 'mkdir -p /mnt/sd_0/ap80dev/player'
adb push "$D/app/player/build/ap80player" "$D/app/player/assets/DejaVuSans.ttf" "$D/app/player/assets/DejaVuSans-Bold.ttf" /mnt/sd_0/ap80dev/player/ >/dev/null
adb push "$D/sdcard/ap80dev/boot.sh" /mnt/sd_0/ap80dev/boot.sh >/dev/null
adb shell 'killall hiby_player.sh hiby_player 2>/dev/null
if pidof ap80player >/dev/null; then echo restart > /tmp/ap80p.cmd; sleep 3; fi
if ! pidof ap80player >/dev/null; then
  printf "#!/bin/sh\ncp /mnt/sd_0/ap80dev/player/ap80player /tmp/ap80player; chmod +x /tmp/ap80player\nexec /tmp/ap80player /mnt/sd_0/ap80dev/player > /tmp/ap80player.log 2>&1\n" > /tmp/runp.sh; chmod +x /tmp/runp.sh
  start-stop-daemon -S -b -x /tmp/runp.sh; sleep 2
fi
pidof ap80player >/dev/null && echo "ap80player running" || { echo "ap80player FAILED"; cat /tmp/ap80player.log; }'
