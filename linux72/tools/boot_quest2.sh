#!/bin/bash
# boot_quest2.sh - bring the Quest 2 from Horizon OS into SteamOS (Linux 7.2) with one command.
#
#   1. Power the headset on (Horizon boots), trigger Singularity root as usual, wake the headset (put it on or cover the proximity sensor).
#   2. ./boot_quest2.sh          prepare only: verify the bundle, check root + display, push files, DRY-RUN the memory claim (no jump)
#      ./boot_quest2.sh --go     the same, then the real jump, Stage 1 -> NBD root -> Stage 2, and wait for the services
#
# Everything the headset needs lives in the boot bundle ($Q2_BOOT_DIR, default /mnt/HDD/quest2-boot): kernel Image, stage 1 initramfs,
# quest_kexec.ko, the five boot-<base>-fan.dtb (made by make_claim_dtbs.sh from the kernel's kexec DTB), kxc.sh, helper scripts, MANIFEST.sha256.
# The SteamOS root is /mnt/HDD/q2steamos/disk.img (served over NBD); all rootfs changes live there.
#
# NOT TESTED END TO END as a script: each command mirrors the procedure that was run by hand (Horizon prep + kxc.sh from HANDOFF.md/NOTES.md,
# Stage 1 -> Stage 2 handoff from the 2026-11-25 session). Any failed check stops the script before the jump.
# Hard rules (HANDOFF.md): never touch enp34s0 / "Wired connection 1"; never write to the headset's UFS; the daemons bind only to 10.42.0.1;
# never edit disk.img while nbd_server.py runs; always stage2_verified=1 shutdown_dsps=1 watchdog_recovery=1 preserve_watchdog=1 execute=1 (kxc.sh does).
set -u
B=${Q2_BOOT_DIR:-/mnt/HDD/quest2-boot}
HOST=10.42.0.1; HEAD=10.42.0.2; CLAIM_MB=${CLAIM_MB:-4608}
GO=0; [ "${1:-}" = "--go" ] && GO=1
TN="python3 -I $B/host/tn.py"
step() { printf '\n== %s\n' "$*"; }
die()  { printf '\nSTOPPED: %s\n' "$*" >&2; exit 1; }
rsu()  { adb shell "su -c '$1'" 2>&1; }            # run as root on Horizon (Singularity root via su)

step "1/8 verify the boot bundle ($B)"
[ -d "$B" ] || die "no boot bundle at $B"
( cd "$B" && sha256sum --quiet -c MANIFEST.sha256 ) || die "bundle checksum mismatch - rebuild it (see linux72/NOTES.md 'Boot bundle')"
echo "bundle OK: $(ls "$B/dtb" | wc -l) device trees, Image $(stat -c %s "$B/Image-linux72") bytes"

step "2/8 headset connected over adb, and root"
command -v adb >/dev/null || die "adb not installed"
adb devices | grep -q 'device$' || die "no adb device (is Horizon booted? is the headset on USB?)"
rsu id | grep -q 'uid=0' || die "no root on Horizon: trigger Singularity first (or root is only reachable via the local socket on 127.0.0.1:1234 - then run the Horizon-side steps by hand, see HANDOFF_STAGE5.md)"
echo "root OK"

step "3/8 Horizon prep: wake display, stop DSP/tracking services, drop caches, compact memory"
adb shell "su -c 'am broadcast -a com.oculus.vrpowermanager.prox_close'" >/dev/null 2>&1; sleep 2
PW=$(rsu 'dumpsys power | grep -E "mWakefulness=|mScreenState="')
echo "$PW" | tr -s ' ' | head -4
echo "$PW" | grep -q 'mWakefulness=Awake' || die "display not awake (mWakefulness) - put the headset on / cover the proximity sensor and retry"
# this Horizon build has no mScreenState line in `dumpsys power`; the panel state is "Display State=ON" in `dumpsys display`
DS=$(rsu 'dumpsys display | grep -E "Display State=|mScreenState="')
echo "$PW$DS" | grep -q -E 'mScreenState=ON|Display State=ON' || die "display not ON (no 'Display State=ON' / mScreenState=ON) - wake it and retry"
echo "display state: $(echo "$DS" | grep -o -E 'Display State=[A-Z]+|mScreenState=[A-Z]+' | sort | uniq -c | tr '\n' ' ')"
for s in $(rsu 'getprop' | grep -o 'init\.svc\.[^]]*\(adsprpcd\|cdsprpcd\|trackingservice\|trackingfidelityservice\)[^]]*' | sed 's/^init\.svc\.//' | sort -u); do
  echo "stopping $s"; rsu "stop $s" >/dev/null
done
rsu 'echo 3 > /proc/sys/vm/drop_caches; echo 1 > /proc/sys/vm/compact_memory' >/dev/null

step "4/8 push the bundle to /data/local/tmp (kxc.sh expects these names)"
adb push "$B/Image-linux72"             /data/local/tmp/Image-linux72 >/dev/null || die "push Image"
adb push "$B/initramfs-linux72.cpio.gz" /data/local/tmp/initramfs-probe.cpio.gz >/dev/null || die "push initramfs"
adb push "$B/quest_kexec.ko"            /data/local/tmp/quest_kexec.ko >/dev/null || die "push module"
adb push "$B/kxc.sh"                    /data/local/tmp/kxc.sh >/dev/null || die "push kxc.sh"
for f in "$B"/dtb/boot-*-fan.dtb; do adb push "$f" /data/local/tmp/ >/dev/null || die "push $f"; done
rsu 'sync'
echo "verifying checksums on the headset..."
for n in Image-linux72 quest_kexec.ko; do
  want=$(grep " $n\$" "$B/MANIFEST.sha256" | cut -d' ' -f1); got=$(rsu "sha256sum /data/local/tmp/$n" | cut -d' ' -f1)
  [ "$want" = "$got" ] || die "checksum mismatch on the headset for $n"
done
want=$(grep ' initramfs-linux72.cpio.gz$' "$B/MANIFEST.sha256" | cut -d' ' -f1); got=$(rsu 'sha256sum /data/local/tmp/initramfs-probe.cpio.gz' | cut -d' ' -f1)
[ "$want" = "$got" ] || die "checksum mismatch on the headset for the initramfs"
echo "files on the headset match the bundle"

step "5/8 dry run: claim $CLAIM_MB MB from Horizon, log the ranges, release them (no jump)"
rsu "sh /data/local/tmp/kxc.sh $CLAIM_MB dry fan" | tail -12
if [ "$GO" != 1 ]; then
  echo; echo "Prepared and dry-run done. Check the claimed ranges above (expect ~2.4-3 GB claimed), then run:  $0 --go"; exit 0
fi

step "6/8 host daemons (bound to $HOST only)"
pgrep -f 'nbd_server.py'      >/dev/null || { ( cd "$B/host" && nohup python3 -I nbd_server.py      > /tmp/q2_nbd_server.log 2>&1 & ); echo "started nbd_server.py"; }
pgrep -f 'kmsg_udp_recv.py'   >/dev/null || { ( cd "$B/host" && nohup python3 -I kmsg_udp_recv.py   > /tmp/q2_kmsg_recv.log 2>&1 & ); echo "started kmsg_udp_recv.py"; }
sleep 2; ss -ltnu 2>/dev/null | grep -E "$HOST:(10809|6666)" | sed 's/  */ /g'

step "7/8 THE JUMP: Horizon -> Linux 7.2 (kxc.sh tries each payload base until one jumps; the adb session dies)"
timeout 150 adb shell "su -c 'sh /data/local/tmp/kxc.sh $CLAIM_MB'" > /tmp/q2_kxc.out 2>&1 &
for i in $(seq 1 90); do ping -c1 -W1 $HEAD >/dev/null 2>&1 && { echo "Stage 1 answers on $HEAD after ~${i}s"; break; }; sleep 2; done
ping -c1 -W2 $HEAD >/dev/null 2>&1 || { tail -5 /tmp/q2_kxc.out; die "Stage 1 did not come up. Headset probably still in Horizon or crashed: check /sys/fs/pstore after the next Singularity, see NOTES.md"; }
sleep 8

step "8/8 Stage 1 -> NBD root -> Stage 2"
$TN $HEAD 23 'busybox nbd-client 10.42.0.1 10809 /dev/nbd0; sleep 2; mkdir -p /mnt/newroot; mount -t btrfs -o rw /dev/nbd0 /mnt/newroot && printf "__TAG_%s__\n" ROOTOK
cat > /run/stage2.sh <<"EOS"
#!/bin/sh
mount --move /proc /mnt/newroot/proc
mount --move /sys /mnt/newroot/sys
mount --move /dev /mnt/newroot/dev
mount --move /run /mnt/newroot/run
exec switch_root /mnt/newroot /sbin/init
EOS
chmod +x /run/stage2.sh' 8 | grep -q '__TAG_ROOTOK__' || die "could not mount the NBD root in Stage 1 (is nbd_server.py running? did disk.img change?)"
for i in $(seq 1 40); do timeout 2 python3 -I -c "import socket;socket.create_connection(('$HEAD',2323),timeout=2)" 2>/dev/null && { echo "Stage 2 (SteamOS) shell on $HEAD:2323"; break; }; sleep 3; done
sleep 50
echo; echo "services (they start by themselves: head tracker, cameras, controller solver, Wi-Fi/Bluetooth):"
$TN $HEAD 2323 'echo HT=$(systemctl is-active q2-headtrack) CAMD=$(systemctl is-active q2-camd) CT=$(systemctl is-active q2-ctltrack) RADIOS=$(systemctl is-active q2-radios) WLAN=$(ip -br link | grep -c wlan0) BT=$(hciconfig 2>/dev/null | grep -c "UP RUNNING")=E' 10 | grep -o 'HT=.*=E'
echo; echo "Done. Shell: telnet $HEAD 2323   |   Start SteamVR:  q2vr start   |   Poweroff cleanly: see BOOT_NEXT_TIME.md"
