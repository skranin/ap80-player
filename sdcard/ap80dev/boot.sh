#!/bin/sh
# ap80dev boot script. Started in background by the patched S92 init script.
# Flags in /mnt/sd_0/ap80dev:  adb = keep ADB on   no_stock = run ap80player instead of the stock player
# Re-exec from /tmp so an SD unmount can't break a running script.
D=/mnt/sd_0/ap80dev
if [ "$1" != "--tmp" ]; then cp $D/boot.sh /tmp/ap80dev_boot.sh; exec /bin/sh /tmp/ap80dev_boot.sh --tmp; fi

L=$D/log; mkdir -p $L
# Log to RAM and copy to the card at checkpoints: an open file on the SD card would block USB storage mode.
exec >>/tmp/ap80dev_boot.log 2>&1
savelog() { [ -d $L ] && cat /tmp/ap80dev_boot.log > $L/boot.log 2>/dev/null; }
echo "=== boot $(date) uptime=$(cut -d' ' -f1 /proc/uptime)"

# --- 1. hardware / system info (every boot, cheap) ---
I=$L/info; mkdir -p $I
for f in cmdline mtd mounts cpuinfo meminfo partitions version; do cat /proc/$f > $I/$f 2>&1; done
dmesg > $I/dmesg 2>&1
cat /proc/bus/input/devices > $I/input 2>&1
(cd /proc/asound && for f in cards pcm devices; do echo "## $f"; cat $f; done) > $I/asound 2>&1
amixer -c 0 contents > $I/amixer_c0 2>&1

# --- 2. full flash + data backup (once) ---
B=$D/backup
if [ ! -f $B/DONE ]; then
  mkdir -p $B; echo "backup start $(date)"
  for m in /dev/mtd[0-9]*; do
    case $m in *ro) continue;; esac
    n=$(basename $m)
    nanddump -f $B/$n.nanddump $m > $B/$n.nanddump.log 2>&1
    dd if=$m of=$B/$n.dd bs=2048 > $B/$n.dd.log 2>&1
  done
  tar -cf $B/data.tar /data 2>$B/data_tar.log
  (cd $B && md5sum *.nanddump *.dd data.tar > MD5SUMS)
  sync; echo ok > $B/DONE; sync; echo "backup done $(date)"
fi
savelog

# --- 3. adb keeper (background) ---
if [ -f $D/adb ]; then
  (
    /etc/init.d/K90adb start
    while [ -f $D/adb ] || [ ! -d $D ]; do
      sleep 5
      grep -q 'adb' /sys/class/android_usb/android0/functions || { echo "re-forcing adb $(date)"; /etc/init.d/K90adb start; }
      pidof adbd >/dev/null || ( /sbin/adbserver.sh & )
    done
  ) &
fi

# --- 4. player ---
run_stock() { echo "starting stock player"; ( /usr/bin/hiby_player.sh & ); }
up() { cut -d. -f1 /proc/uptime; }
if [ -f $D/no_stock ]; then
  P=$D/player
  if [ -f $P/ap80player ]; then
    fails=0
    while :; do
      cp $P/ap80player /tmp/ap80player && chmod +x /tmp/ap80player
      t0=$(up)
      /tmp/ap80player $P > /tmp/ap80player.log 2>&1; rc=$?
      [ -d $L ] && cp /tmp/ap80player.log $L/ap80player_last.log
      echo "ap80player exited rc=$rc after $(( $(up) - t0 ))s"; savelog
      case $rc in
        0)  break ;;                         # quit on purpose / power off
        20) run_stock; break ;;              # "Switch to stock player"
        21) fails=0; continue ;;             # restart requested (deploy)
      esac
      if [ $(( $(up) - t0 )) -lt 30 ]; then fails=$((fails+1)); else fails=0; fi
      if [ $fails -ge 3 ]; then echo "ap80player crash loop -> stock"; run_stock; break; fi
      sleep 1
    done
  else
    run_stock
  fi
fi
echo "=== boot.sh end $(date)"
savelog; sync
