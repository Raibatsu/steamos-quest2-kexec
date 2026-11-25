#!/bin/bash
# claim_sweep.sh - how much RAM can the loader claim from the running Horizon? Dry runs only (claim, log, release: nothing jumps). Run with Horizon booted and rooted (adb su),
# ideally right after Singularity. Prints "extra claim" per setting. Usage: claim_sweep.sh [extra prep: 'prep' to free memory first]
B=${Q2_BOOT_DIR:-/mnt/HDD/quest2-boot}
rsu() { adb shell "su -c '$1'" 2>&1; }
adb shell "su -c id" 2>&1 | grep -q uid=0 || { echo "no root on Horizon"; exit 1; }
adb push "$B/quest_kexec.ko" /data/local/tmp/quest_kexec.ko >/dev/null; adb push "$B/kxc.sh" /data/local/tmp/kxc.sh >/dev/null
adb push "$B/Image-linux72" /data/local/tmp/Image-linux72 >/dev/null; adb push "$B/initramfs-linux72.cpio.gz" /data/local/tmp/initramfs-probe.cpio.gz >/dev/null
for f in "$B"/dtb/boot-130-fan.dtb; do adb push "$f" /data/local/tmp/ >/dev/null; done
if [ "${1:-}" = prep ]; then rsu 'am kill-all; sync; for i in 1 2 3; do echo 3 > /proc/sys/vm/drop_caches; echo 1 > /proc/sys/vm/compact_memory; sleep 2; done' >/dev/null; fi
rsu 'grep -E "MemTotal|MemFree|MemAvailable|Slab|SUnreclaim|PageTables|KernelStack|CmaTotal" /proc/meminfo | tr -s " " | tr "\n" ";"'; echo
for cfg in ${CONFIGS:-"4096 1" "2048 1" "2048 4" "1024 4" "512 4"}; do
  set -- $cfg
  echo "== step $1 KiB, passes $2"
  rsu "lsmod | grep -q quest_kexec && rmmod quest_kexec; dmesg -C; STEP=$1 PASSES=$2 sh /data/local/tmp/kxc.sh 5120 dry fan" | grep -E "extra claim|tried/|failed|error" | sed "s/^\[ *[0-9.]*\] quest_kexec: //" | cut -c1-200
done
