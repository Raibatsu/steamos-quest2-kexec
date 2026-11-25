#!/bin/bash
# kexec_l2l.sh - Linux -> Linux kexec from a running Stage 2, no Horizon, no Singularity (about 3 minutes, mostly waiting).
#   usage: kexec_l2l.sh [<kexec.dtb> [<Image> [<initramfs>]]]
#   defaults: the kernel tree's build output (/mnt/HDD/quest2-linux72/out-up) DTB, and the boot bundle's Image and initramfs.
# Steps (the manual procedure of 2026-11-25, now scripted): fetch the RUNNING kernel's /sys/firmware/fdt, build the kexec DTB for a free RAM
# base (make_q2kexec_dtb.py), serve the files from this computer, stage them in Stage 2 with q2kexec -l (a load check that does not jump),
# execute, wait for Stage 1, restart the host daemons, attach the NBD root and switch to Stage 2.
# Run this only from a healthy Stage 2 (telnet 10.42.0.2:2323 works). Stop VR / the camera daemon first (systemctl stop q2-ctltrack q2-camd).
set -u
B=${Q2_BOOT_DIR:-/mnt/HDD/quest2-boot}
DTB=${1:-/mnt/HDD/quest2-linux72/out-up/arch/arm64/boot/dts/qcom/sm8250-oculus-hollywood-kexec.dtb}
IMG=${2:-$B/Image-linux72}; RD=${3:-$B/initramfs-linux72.cpio.gz}
HOST=10.42.0.1; HEAD=10.42.0.2; TN="python3 -I $B/host/tn.py"
die() { echo "STOPPED: $*" >&2; exit 1; }
[ -f "$DTB" ] && [ -f "$IMG" ] && [ -f "$RD" ] || die "missing DTB/Image/initramfs ($DTB $IMG $RD)"
W=$(mktemp -d /tmp/q2l2l.XXXXXX); trap 'kill $HTTP 2>/dev/null; rm -rf "$W"' EXIT

echo "== 1/6 fetch the running kernel's device tree"
python3 -I - "$W/live.dtb" <<'EOF' &
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('10.42.0.1', 9000)); s.listen(1); s.settimeout(60)
c, _ = s.accept(); d = b''
while True:
    b = c.recv(65536)
    if not b: break
    d += b
open(sys.argv[1], 'wb').write(d)
EOF
LST=$!; sleep 1
$TN $HEAD 2323 '/usr/local/bin/busybox nc 10.42.0.1 9000 < /sys/firmware/fdt; echo sent' 10 | grep -q sent || die "could not read /sys/firmware/fdt (is Stage 2 up?)"
wait $LST; [ -s "$W/live.dtb" ] || die "empty live device tree"
BASE=$(python3 -I "$B/host/make_q2kexec_dtb.py" "$W/live.dtb" "$DTB" "$IMG" "$RD" "$W/kx.dtb") || die "make_q2kexec_dtb.py failed"
echo "payload base $BASE"

echo "== 2/6 serve the files on $HOST:8000 and stage them in Stage 2 (load check, no jump)"
cp "$IMG" "$W/Image"; cp "$RD" "$W/initrd.gz"; cp "$B/q2kexec" "$W/q2kexec"
# a stale server from an earlier run (serving a deleted directory) would keep the port: kill any listener on it first
for pid in $(ss -ltnp 2>/dev/null | grep "$HOST:8000" | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -u); do kill $pid 2>/dev/null; done; sleep 1
# exec: so that $! is the python process itself and the EXIT trap really stops it
( cd "$W" && exec python3 -m http.server 8000 --bind $HOST >/dev/null 2>&1 ) & HTTP=$!; sleep 1
OUT=$($TN $HEAD 2323 "B=/usr/local/bin/busybox; mkdir -p /run/kx; cd /run/kx; for f in Image initrd.gz kx.dtb q2kexec; do \$B wget -q -O \$f http://$HOST:8000/\$f || echo WGET_FAILED \$f; done; ls -la /run/kx | tail -6; chmod +x q2kexec; ./q2kexec -l -B $BASE -D kx.dtb Image initrd.gz; echo rc=\$?" 25)
echo "$OUT" | grep -q 'rc=0' || { echo "$OUT" | tail -12; die "load check failed (the kernel was NOT replaced)"; }
echo "load check OK"

echo "== 3/6 jump"
$TN $HEAD 2323 "sync; cd /run/kx; (nohup ./q2kexec -B $BASE -D kx.dtb Image initrd.gz > /run/kx.log 2>&1 &); echo executing" 3 >/dev/null
for i in $(seq 1 20); do ping -c1 -W1 $HEAD >/dev/null 2>&1 || break; sleep 1; done
for i in $(seq 1 60); do ping -c1 -W1 $HEAD >/dev/null 2>&1 && { echo "Stage 1 answers"; break; }; sleep 2; done
ping -c1 -W2 $HEAD >/dev/null 2>&1 || die "Stage 1 did not come up after the jump"
sleep 12

echo "== 4/6 restart the host daemons (they must accept a fresh connection)"
for pid in $(ps -eo pid,args | awk '$2 ~ /python3/ && ($0 ~ /nbd_server\.py|kmsg_udp_recv\.py/) {print $1}'); do kill $pid 2>/dev/null; done; sleep 1
( cd "$B/host" && nohup python3 -I nbd_server.py    > /tmp/q2_nbd_server.log 2>&1 & )
( cd "$B/host" && nohup python3 -I kmsg_udp_recv.py > /tmp/q2_kmsg_recv.log 2>&1 & ); sleep 2

echo "== 5/6 Stage 1 -> NBD root -> Stage 2"
$TN $HEAD 23 'busybox nbd-client 10.42.0.1 10809 /dev/nbd0; sleep 2; mkdir -p /mnt/newroot; mount -t btrfs -o rw /dev/nbd0 /mnt/newroot && printf "__TAG_%s__\n" ROOTOK
cat > /run/stage2.sh <<"EOS"
#!/bin/sh
mount --move /proc /mnt/newroot/proc
mount --move /sys /mnt/newroot/sys
mount --move /dev /mnt/newroot/dev
mount --move /run /mnt/newroot/run
exec switch_root /mnt/newroot /sbin/init
EOS
chmod +x /run/stage2.sh' 8 | grep -q '__TAG_ROOTOK__' || die "could not mount the NBD root in Stage 1"
for i in $(seq 1 40); do timeout 2 python3 -I -c "import socket;socket.create_connection(('$HEAD',2323),timeout=2)" 2>/dev/null && { echo "Stage 2 shell is up on $HEAD:2323"; break; }; sleep 3; done

echo "== 6/6 wait for the boot services"
for i in $(seq 1 30); do r=$($TN $HEAD 2323 'echo CAMD=$(systemctl is-active q2-camd)=E' 6 | grep -o 'CAMD=[a-z]*=E'); [ "$r" = "CAMD=active=E" ] && break; sleep 5; done
$TN $HEAD 2323 'echo HT=$(systemctl is-active q2-headtrack) CAMD=$(systemctl is-active q2-camd) CT=$(systemctl is-active q2-ctltrack) RADIOS=$(systemctl is-active q2-radios) WLAN=$(ip -br link | grep -c wlan0) BT=$(hciconfig 2>/dev/null | grep -c "UP RUNNING")=E' 10 | grep -o 'HT=.*=E'
