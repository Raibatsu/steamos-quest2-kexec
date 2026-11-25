#!/bin/bash
# horizon_probe.sh - run q2_snapshot.sh on the headset (Horizon over adb root, or our Linux over telnet) and pull the result to this computer, to compare the two.
#   horizon_probe.sh horizon snapshot|audio|ctl|dyndbg ...   Horizon (Quest OS) booted and rooted with Singularity, adb connected
#   horizon_probe.sh linux   snapshot|audio|ctl|dyndbg ...   our Stage 2 (telnet 10.42.0.2:2323), via the host's 10.42.0.1 web server
#   horizon_probe.sh compare <horizon-dir> <linux-dir>       side-by-side summary of what differs (clocks, regulators, gpios, pinctrl, props, asoc, regmaps)
# Output goes to /mnt/HDD/quest2-probe/<system>-<mode>-<time>/ (outside the git repo: it holds device logs, never commit it).
# Read-only on the device except dynamic-debug / ftrace flags that q2_snapshot.sh sets and clears again. Never writes partitions.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
B=${Q2_BOOT_DIR:-/mnt/HDD/quest2-boot}
OUTROOT=${Q2_PROBE_DIR:-/mnt/HDD/quest2-probe}
die() { echo "STOPPED: $*" >&2; exit 1; }
sys=${1:-}; mode=${2:-}
[ -n "$sys" ] || { sed -n 2,9p "$0"; exit 1; }

if [ "$sys" = compare ]; then
  H=${2:?horizon dir}; L=${3:?linux dir}
  for f in props clk_summary regulator_summary gpio pinctrl power_supply remoteproc alsa asoc regmap_list modules; do
    [ -f "$H/$f" ] && [ -f "$L/$f" ] || continue
    n=$(diff <(sed 's/[0-9a-f]\{8,\}//g' "$H/$f") <(sed 's/[0-9a-f]\{8,\}//g' "$L/$f") | grep -c '^[<>]')
    echo "== $f: $n differing lines (diff -y -W 200 $H/$f $L/$f | less)"
  done
  exit 0
fi

TS=$(date +%Y%m%d-%H%M%S)
DEST="$OUTROOT/$sys-${mode:-snapshot}-$TS"; mkdir -p "$DEST"
shift 2
ARGS="$*"
# a 3 s 1 kHz 48 kHz S16 stereo tone for the audio trace
python3 -I - "$DEST/tone.wav" <<'EOF'
import sys, struct, math
n = 48000 * 3
d = b''.join(struct.pack('<hh', int(8000 * math.sin(2 * math.pi * 1000 * i / 48000)), int(8000 * math.sin(2 * math.pi * 1000 * i / 48000))) for i in range(n))
open(sys.argv[1], 'wb').write(b'RIFF' + struct.pack('<I', 36 + len(d)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 48000, 192000, 4, 16) + b'data' + struct.pack('<I', len(d)) + d)
EOF

if [ "$sys" = horizon ]; then
  command -v adb >/dev/null || die "adb not installed"
  adb devices | grep -q 'device$' || die "no adb device (Horizon booted? USB connected?)"
  adb shell "su -c id" 2>&1 | grep -q 'uid=0' || die "no root on Horizon: run Singularity first"
  R=/data/local/tmp/q2snap
  adb shell "rm -rf $R; mkdir -p $R" >/dev/null
  adb push "$HERE/q2_snapshot.sh" /data/local/tmp/q2_snapshot.sh >/dev/null || die "push script"
  adb push "$DEST/tone.wav" $R/tone.wav >/dev/null
  echo "running q2_snapshot.sh $mode on Horizon (this can take a minute)..."
  timeout 600 adb shell "su -c 'sh /data/local/tmp/q2_snapshot.sh $mode $R $ARGS'" 2>&1 | tail -5
  adb pull $R/. "$DEST/" >/dev/null 2>&1 || die "pull results"
elif [ "$sys" = linux ]; then
  TN="python3 -I $B/host/tn.py 10.42.0.2 2323"
  W=$(mktemp -d /tmp/q2probe.XXXXXX); cp "$HERE/q2_snapshot.sh" "$DEST/tone.wav" "$W/"
  ( cd "$W" && exec python3 -m http.server 8001 --bind 10.42.0.1 >/dev/null 2>&1 ) & HTTP=$!
  python3 -I - "$DEST" <<'EOF' &
import socket, sys, tarfile, io
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('10.42.0.1', 9100)); s.listen(1); s.settimeout(600)
c, _ = s.accept(); d = b''
while True:
    b = c.recv(65536)
    if not b: break
    d += b
tarfile.open(fileobj=io.BytesIO(d)).extractall(sys.argv[1], filter='data')
EOF
  RX=$!
  sleep 1
  $TN "R=/run/q2snap; rm -rf \$R; mkdir -p \$R; B=/usr/local/bin/busybox; \$B wget -q -O /run/q2_snapshot.sh http://10.42.0.1:8001/q2_snapshot.sh; \$B wget -q -O \$R/tone.wav http://10.42.0.1:8001/tone.wav; sh /run/q2_snapshot.sh $mode \$R $ARGS > /run/q2snap.log 2>&1; tail -2 /run/q2snap.log; cd \$R && tar c . | \$B nc 10.42.0.1 9100; echo SENT" 580 | tail -4
  wait $RX 2>/dev/null; kill $HTTP 2>/dev/null; rm -rf "$W"
else
  die "first argument must be horizon, linux or compare"
fi
echo "result: $DEST ($(ls "$DEST" | wc -l) files)"
