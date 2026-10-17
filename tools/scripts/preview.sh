#!/bin/sh
# Headless UI preview under qemu: renders pages in portrait and landscape into /p/screenshots/preview/.
# Run inside the ap80-tc image: docker run --rm -v $PWD:/p ap80-tc sh /p/tools/scripts/preview.sh
mkdir -p /mnt; ln -sfn /p/scratch_test/sd /mnt/sd_0; mkdir -p /mnt/sd_0/ap80dev /tmp/shots
rm -f /mnt/sd_0/ap80dev/player_state.txt
# TEXT=0..3 picks the text size; PLAYALL_Y = y of the "Play all" row at that size
[ -n "$TEXT" ] && echo "text_size=$TEXT" > /mnt/sd_0/ap80dev/player_state.txt
S=${TEXT:-0}; PA=${PLAYALL_Y:-125}
qemu-mipsel-static /p/app/player/build/ap80player /p/app/player/assets > /tmp/out.log 2>&1 &
PID=$!; sleep 3
c() { echo "$1" > /tmp/ap80p.cmd; sleep 1; }
for r in 0 1; do
  c "rot $r"; W=360; H=480; [ $r = 1 ] && { W=480; H=360; }
  T=$((H - 40))                                     # tray row
  c "tap $((W/2)) $T"; c "shot /tmp/shots/t${S}_library_r$r.ppm"
  c "tap $((W/2)) $((34 + 60 + 70 + 35))"; c "shot /tmp/shots/t${S}_folder_r$r.ppm"   # first folder row
  c "tap 100 $PA"; c "shot /tmp/shots/t${S}_player_r$r.ppm"         # Play all
  c "tap $((W*5/6)) $T"; c "shot /tmp/shots/t${S}_settings_r$r.ppm"
  c "drag 200 $((H-120)) 200 60"; c "shot /tmp/shots/t${S}_settings2_r$r.ppm"
done
kill -0 $PID 2>/dev/null && echo ALIVE || echo DIED; c quit
cat /tmp/out.log | tail -5
for f in /tmp/shots/*.ppm; do python3 /p/tools/ppm2png.py "$f" "/p/screenshots/preview/$(basename "$f" .ppm).png"; done
