#!/bin/sh
apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq gdb-multiarch >/dev/null 2>&1
mkdir -p /mnt; ln -sfn /p/scratch_test/sd /mnt/sd_0; mkdir -p /mnt/sd_0/ap80dev; rm -f /mnt/sd_0/ap80dev/player_state.txt
qemu-mipsel-static -g 1234 /p/app/player/build/ap80player_dbg /p/app/player/assets > /tmp/out.log 2>&1 &
( sleep 6; for c in "tap 180 174" "tap 180 174" "tap 180 174"; do echo "$c" > /tmp/ap80p.cmd; sleep 2; done ) &
gdb-multiarch -q -batch -ex "set pagination off" -ex "target remote :1234" -ex "continue" -ex "bt 15" -ex "info threads" -ex "thread apply all bt 8" /p/app/player/build/ap80player_dbg 2>&1 | grep -vE "^\[New|warning" | tail -60
