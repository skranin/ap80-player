#!/bin/sh
# Usage: build_upt.sh <stock_upt> <overlay_dir|-> <out_upt> <version_string>
set -e
STOCK=$1; OVL=$2; OUT=$3; VER=$4
W=/tmp/bu; rm -rf $W; mkdir -p $W/iso $W/root
xorriso -osirrox on -indev $STOCK -extract / $W/iso >/dev/null 2>&1
chmod -R u+w $W/iso
ubireader_extract_files -k -o $W/root $W/iso/SYSTEM.UBI >/dev/null 2>&1 || true
if [ "$OVL" != "-" ]; then
  # copy overlay files, keep stock ownership of replaced files
  (cd $OVL && find . -type f) | while read f; do
    t=$W/root/$f
    own=$(stat -c %u:%g $(dirname $t))
    [ -e $t ] && own=$(stat -c %u:%g $t)
    cp $OVL/$f $t; chown $own $t; chmod 755 $t
    echo "overlay: $f"
  done
fi
mkfs.ubifs -r $W/root -m 2048 -e 126976 -c 480 -x lzo -F -o $W/iso/SYSTEM.UBI
SUM=$(md5sum $W/iso/SYSTEM.UBI | cut -d' ' -f1)
sed -i "/name=rootfs/,/}/ s/md5=.*/md5=$SUM/" $W/iso/UPDATE.TXT
sed -i "s/ver=.*/ver=$VER/" $W/iso/VERSION.TXT
grep -q "$SUM" $W/iso/UPDATE.TXT
md5sum $W/iso/UIMAGE.BIN | grep -q "$(sed -n '/name=kernel/,/}/ s/.*md5=//p' $W/iso/UPDATE.TXT | tr -d '\r ')"
cd $W/iso && genisoimage -quiet -V CDROM -sysid LINUX -o $OUT SYSTEM.UBI UIMAGE.BIN UPDATE.TXT VERSION.TXT _GITIGNO
echo "built $OUT rootfs_md5=$SUM"; cat $W/iso/VERSION.TXT
