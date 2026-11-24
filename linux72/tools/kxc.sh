#!/system/bin/sh
# Run as root on the headset (Horizon). usage: kxc.sh <claim_mb> [dry] [dtb-suffix]
#   dry   : stage + log the claimed ranges, then release them (safe, no jump)
#   no dry: kexec into Linux, trying each payload base until one jumps
# Needs in /data/local/tmp: quest_kexec.ko, Image-linux72, initramfs-*.cpio.gz, boot-<base>-<suffix>.dtb
# Horizon prep (see NOTES.md): wake display (mWakefulness=Awake AND mScreenState=ON), am kill-all,
# drop_caches + compact_memory. claim_step_kb=4096 + claim_relax=1 gave the most RAM (~3.7 GB of 6 GB).
cd /data/local/tmp
MB=${1:-4608}; SUF=${3:-fan}
COMMON="image=/data/local/tmp/Image-linux72 initrd=/data/local/tmp/initramfs-probe.cpio.gz stage2_verified=1 claim_mb=$MB claim_step_kb=4096 claim_relax=1"
if [ "$2" = dry ]; then
  insmod ./quest_kexec.ko $COMMON dtb=/data/local/tmp/boot-130-$SUF.dtb payload_base=0x130000000 2>&1
  dmesg | grep quest_kexec | tail -40
  lsmod | grep -q quest_kexec && rmmod quest_kexec && echo released
  exit 0
fi
for pb in 130 100 140 160 180; do
  insmod ./quest_kexec.ko $COMMON dtb=/data/local/tmp/boot-$pb-$SUF.dtb payload_base=0x${pb}000000 shutdown_dsps=1 watchdog_recovery=1 preserve_watchdog=1 execute=1 2>&1
  echo "base $pb: $(dmesg | grep quest_kexec | tail -1)"
  sleep 3
done
