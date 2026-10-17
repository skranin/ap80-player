#!/bin/sh
# find code referencing a string in hiby_player and print surrounding disassembly
B=/p/firmware/work/rootfs11/usr/bin/hiby_player
for S in "failed to access ioctl START_DMA" "failed to access ioctl GET_EMPTY_BUF" "failed to access ioctl SET_SLEEP"; do
  OFF=$(grep -abo "$S" $B | head -1 | cut -d: -f1)
  # file offset -> vaddr using program headers
  VA=$(mipsel-linux-gnu-readelf -lW $B | awk -v off=$OFF '$1=="LOAD"{o=strtonum($2);v=strtonum($3);s=strtonum($5); if(off>=o && off<o+s){printf "%d", v+off-o-14}}')
  # string includes "(hgl_fb: W) " prefix 12 chars before - search actual start
  START=$(python3 -c "
import sys;d=open('$B','rb').read();i=d.find(b'$S');j=d.rfind(b'\0',0,i)+1;print(j)")
  VA=$(mipsel-linux-gnu-readelf -lW $B | awk -v off=$START '$1=="LOAD"{o=strtonum($2);v=strtonum($3);s=strtonum($5); if(off>=o && off<o+s){printf "%d", v+off-o}}')
  HI=$(( (VA + 0x8000) >> 16 )); LO=$(( VA & 0xffff ))
  printf "== %s  va=0x%x hi=0x%x lo=0x%x\n" "$S" $VA $HI $LO
  mipsel-linux-gnu-objdump -d --no-show-raw-insn $B > /tmp/dis.txt 2>/dev/null || true
  LOHEX=$(printf "0x%x" $LO); [ $LO -ge 32768 ] && LOHEX=$(printf -- "-0x%x" $((65536-LO)))
  grep -n "addiu.*,$LOHEX\$" /tmp/dis.txt | head -3 | while IFS=: read ln rest; do
    sed -n "$((ln-40)),$((ln+3))p" /tmp/dis.txt | grep -E 'lui|li|ori|addiu|jalr|lw.*ioctl|ioctl' ; echo ----; done
done
