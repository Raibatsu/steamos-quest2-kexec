#!/system/bin/sh
# usage: kxc.sh <claim_mb> [dry]
cd /data/local/tmp
MB=${1:-2560}
COMMON="image=/data/local/tmp/Image-linux72 initrd=/data/local/tmp/initramfs-probe.cpio.gz stage2_verified=1 claim_mb=$MB"
if [ "$2" = dry ]; then
  insmod ./quest_kexec.ko $COMMON dtb=/data/local/tmp/boot-130-claim.dtb payload_base=0x130000000 2>&1
  dmesg | grep quest_kexec | tail -40
  lsmod | grep -q quest_kexec && rmmod quest_kexec && echo released
  exit 0
fi
for pb in 130 100 140 160 180; do
  insmod ./quest_kexec.ko $COMMON dtb=/data/local/tmp/boot-$pb-claim.dtb payload_base=0x${pb}000000 shutdown_dsps=1 watchdog_recovery=1 preserve_watchdog=1 execute=1 2>&1
  echo "base $pb: $(dmesg | grep quest_kexec | tail -1)"
  sleep 3
done
