#!/bin/sh
# Extract stock UBIFS as root (keeps perms), rebuild with mkfs.ubifs, re-extract, compare.
set -e
IMG=$1; OUT=$2
W=/tmp/rt; rm -rf $W; mkdir -p $W
ubireader_extract_files -k -o $W/a $IMG >/dev/null 2>&1 || true
tar -C $W/a --numeric-owner -cpzf $OUT/rootfs_stock.tgz .
mkfs.ubifs -r $W/a -m 2048 -e 126976 -c 480 -x lzo -F -o $W/new.ubifs
ubireader_extract_files -k -o $W/b $W/new.ubifs >/dev/null 2>&1 || true
manifest() { cd $1 && find . -printf '%M %U:%G %s %l %p\n' | sort -k6 > $2; find . -type f -exec md5sum {} + | sort -k2 >> $2; }
manifest $W/a $W/ma; manifest $W/b $W/mb
if diff -q $W/ma $W/mb >/dev/null; then echo "ROUNDTRIP IDENTICAL"; else echo "DIFF:"; diff $W/ma $W/mb | head -20; fi
ls -la $IMG $W/new.ubifs; cp $W/new.ubifs $OUT/roundtrip.ubifs
echo files: $(find $W/a | wc -l); ls -la $W/a/bin/busybox $W/a/dev | head
