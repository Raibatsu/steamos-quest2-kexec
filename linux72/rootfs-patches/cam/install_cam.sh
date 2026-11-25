#!/bin/sh
# Installs the Horizon-style camera exposure support on the headset rootfs (run as root, files fetched next to this script or in /run):
#   - allows SET_DATA 0xa1/0xa3/0xef in q2headtrack's sbcam trigger whitelist (idempotent; backup /usr/bin/q2headtrack.prea1)
#   - /usr/bin/q2camae (auto-exposure daemon) and q2-camae.service, written to the LOWER layer of the rootfs (the /etc overlay is mounted after systemd reads units)
set -e
D=${1:-/run}
grep -q "0xa1, 0xa3, 0xef" /usr/bin/q2headtrack || { cp -n /usr/bin/q2headtrack /usr/bin/q2headtrack.prea1; sed -i "s/b\[3\] in (0xdd, 0xc7, 0x64)/b[3] in (0xdd, 0xc7, 0x64, 0xa1, 0xa3, 0xef)/" /usr/bin/q2headtrack; echo "whitelist patched"; }
install -m 755 $D/q2camae /usr/bin/q2camae
mkdir -p /mnt/rb && mountpoint -q /mnt/rb || mount --bind / /mnt/rb
cat > /mnt/rb/etc/systemd/system/q2-camae.service <<'UNIT'
[Unit]
Description=Quest 2 camera auto-exposure (Horizon-style 0xef/0xa3/0xa1)
After=q2-headtrack.service q2-camd.service
Wants=q2-headtrack.service q2-camd.service
ConditionPathExists=/usr/bin/q2camae
ConditionPathExists=!/var/lib/q2/nocameras

[Service]
Type=simple
ExecStart=/usr/bin/q2camae
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
mkdir -p /mnt/rb/etc/systemd/system/multi-user.target.wants
ln -sf /etc/systemd/system/q2-camae.service /mnt/rb/etc/systemd/system/multi-user.target.wants/q2-camae.service
umount /mnt/rb 2>/dev/null || true
systemctl daemon-reload 2>/dev/null || true
echo installed
