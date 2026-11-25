#!/bin/sh
# Installs q2audio and a boot service that starts the audio DSP (its firmware is requested before the root filesystem exists, so the kernel's own attempt fails) and thereby
# brings up the CM710x/AK4331 codecs and the speaker amp; before this runs the codec sits unconfigured and the speakers hiss. Lower layer for the unit (the /etc overlay mounts late).
set -e
D=${1:-/run}
install -m 755 $D/q2audio /usr/bin/q2audio
mkdir -p /mnt/rb && mountpoint -q /mnt/rb || mount --bind / /mnt/rb
cat > /mnt/rb/etc/systemd/system/q2-audio.service <<'UNIT'
[Unit]
Description=Quest 2 audio: start the ADSP and bring up the codecs
After=local-fs.target
ConditionPathExists=/usr/bin/q2audio
ConditionPathExists=/lib/firmware/qcom/sm8250/hollywood/adsp.mdt

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/bin/q2audio start

[Install]
WantedBy=multi-user.target
UNIT
mkdir -p /mnt/rb/etc/systemd/system/multi-user.target.wants
ln -sf /etc/systemd/system/q2-audio.service /mnt/rb/etc/systemd/system/multi-user.target.wants/q2-audio.service
umount /mnt/rb 2>/dev/null || true
systemctl daemon-reload 2>/dev/null || true
echo installed
