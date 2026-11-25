#!/bin/bash
# Build boot-<base>-<suffix>.dtb for every payload base from the kernel's kexec DTB:
#  - linux,initrd-start/end for payload_base + rd (rd = PAGE_ALIGN(text_offset + Image header memory size), read from the Image)
#  - the memory node's reg replaced by a 1024-entry placeholder (each entry 51a0c0de c0de0000 0 0) that loader.c
#    (claim_mb=N) overwrites at insmod time with the payload destination + the RAM ranges it claimed from Horizon.
# usage: make_claim_dtbs.sh <sm8250-oculus-hollywood-kexec.dtb> <Image> <initramfs.cpio.gz> <outdir> [suffix=fan] [extra bootarg]
set -e
DTB=$1; IMG=$2; RD=$3; OUT=$4; SUF=${5:-fan}; EXTRA=$6
RS=$(stat -c %s "$RD")
RDOFF=$(python3 -I -c "
import struct,sys
b=open('$IMG','rb').read(64); off,size=struct.unpack_from('<QQ',b,8); print((off+size+4095)&~4095)")
cells=$(python3 -I -c 'print(" ".join(["51a0c0de","c0de0000","0","0"]*1024))')
mkdir -p "$OUT"
for b in ${BASES:-100 130 140 160 180}; do   # BASES="110 120 ..." overrides the candidate payload bases (units of 0x1000000, i.e. 130 = 0x130000000)
  f="$OUT/boot-$b-$SUF.dtb"; cp "$DTB" "$f"
  s=$((0x${b}000000 + RDOFF)); e=$((s + RS))
  fdtput -t u "$f" /chosen linux,initrd-start $((s>>32)) $((s&0xffffffff))
  fdtput -t u "$f" /chosen linux,initrd-end $((e>>32)) $((e&0xffffffff))
  fdtput -t x "$f" /memory@80000000 reg $cells
  if [ -n "$EXTRA" ]; then cur=$(fdtget "$f" /chosen bootargs); fdtput -ts "$f" /chosen bootargs "$cur $EXTRA"; fi
done
echo "rd offset $(printf '%#x' $RDOFF), initramfs $RS bytes"
