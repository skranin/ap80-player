#!/bin/sh
# Run ap80player headless under qemu with a fake SD card and replay touch commands.
mkdir -p /mnt; ln -sfn /p/scratch_test/sd /mnt/sd_0; mkdir -p /mnt/sd_0/ap80dev
rm -f /mnt/sd_0/ap80dev/player_state.txt
qemu-mipsel-static ${QEMU_GDB:+-g 1234} /p/app/player/build/ap80player_dbg /p/app/player/assets > /tmp/out.log 2>&1 &
PID=$!; sleep 3
for c in "tap 180 174" "tap 180 174" "tap 180 174"; do echo "$c" > /tmp/ap80p.cmd; sleep 2; done
sleep 2; kill -0 $PID 2>/dev/null && echo "ALIVE" || { wait $PID; echo "DIED rc=$?"; }
cat /tmp/out.log; cat /mnt/sd_0/ap80dev/player_state.txt 2>/dev/null
