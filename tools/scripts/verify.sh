#!/bin/sh
set -e
P=/p; W=/tmp/v; rm -rf $W; mkdir -p $W/s $W/m $W/si $W/mi
xorriso -osirrox on -indev $P/firmware/stock_1.1/update.upt -extract / $W/si >/dev/null 2>&1
xorriso -osirrox on -indev $P/firmware/mod/out/update.upt -extract / $W/mi >/dev/null 2>&1
ubireader_extract_files -k -o $W/s $W/si/SYSTEM.UBI >/dev/null 2>&1 || true
ubireader_extract_files -k -o $W/m $W/mi/SYSTEM.UBI >/dev/null 2>&1 || true
echo "== ISO file list (mod)"; ls -la $W/mi
echo "== UPDATE.TXT diff"; diff $W/si/UPDATE.TXT $W/mi/UPDATE.TXT || true
echo "== rootfs diff (names/perms/content)"; diff -r --no-dereference $W/s $W/m 2>&1 | head; 
(cd $W/s; find . -printf '%M %U:%G %p\n' | sort -k3) > $W/a; (cd $W/m; find . -printf '%M %U:%G %p\n' | sort -k3) > $W/b; diff $W/a $W/b && echo "perms/owners identical"
echo "== md5 check"; md5sum $W/mi/SYSTEM.UBI $W/mi/UIMAGE.BIN; cat $W/mi/UPDATE.TXT | grep md5
echo "== run hook with device busybox (qemu)"
R=$W/m; mkdir -p $R/mnt/sd_0/ap80dev; echo 'echo HOOK_RAN' > $R/mnt/sd_0/ap80dev/boot.sh; touch $R/mnt/sd_0/ap80dev/no_stock
sed 's#/mnt/sd_0#'$R'/mnt/sd_0#; s#/tmp/ap80dev_boot.log#/dev/stdout#; s#/bin/sh \$DEV#'"qemu-mipsel-static -L $R $R/bin/busybox sh"' $DEV#' $R/etc/init.d/S92_03_start_music_player > $W/hook.sh
qemu-mipsel-static -L $R $R/bin/busybox sh $W/hook.sh start; sleep 1; echo
qemu-mipsel-static -L $R $R/bin/busybox sleep 0.5 && echo "fractional sleep OK"
