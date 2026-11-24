#!/bin/bash
# Build boot-<base>-claim.dtb from the packed boot-<base>-linux72.dtb: replace the memory node's reg with a
# 256-entry placeholder (each entry = 51a0c0de c0de0000 0 0) that loader.c (claim_mb=N) overwrites at insmod time
# with the payload destination + the RAM ranges it claimed from Horizon. Usage: make_claim_dtbs.sh <outdir> [extra bootarg]
set -e
OUT=${1:-/mnt/HDD/quest2-linux72/out}; EXTRA=$2
cells=$(python3 -I -c 'print(" ".join(["51a0c0de","c0de0000","0","0"]*256))')
for b in 100 130 140 160 180; do
  cp "$OUT/boot-$b-linux72.dtb" "$OUT/boot-$b-claim.dtb"
  fdtput -t x "$OUT/boot-$b-claim.dtb" /memory@80000000 reg $cells
  if [ -n "$EXTRA" ]; then
    cur=$(fdtget "$OUT/boot-$b-claim.dtb" /chosen bootargs)
    fdtput -ts "$OUT/boot-$b-claim.dtb" /chosen bootargs "$cur $EXTRA"
  fi
done
