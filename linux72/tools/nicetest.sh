# usage: nicetest.sh "<frames>" tag   (runs on the device)
F="$1"; T="$2"
systemctl stop q2-camd q2-ctltrack q2-headtrack 2>/dev/null; sleep 2
(timeout 50 python3 /run/sbproxy5.py 40 $F 03000164 > /run/sbp_$T.out 2>&1 &); sleep 3
timeout 45 /usr/bin/q2camlive > /run/camlive_$T.out 2>&1; sleep 3
grep -v 'type=0xfe' /run/sbp_$T.out | grep -E 'MCU' | grep -v 'not configured\|stop_streaming\|i2c chain\|Failed writing\|Failed to stop\|type=0x7d\|type=0xee' | cut -c1-200 | head -6
echo "RESULT $T: $(grep 'images in total' /run/camlive_$T.out | cut -c20-80)"
grep -a 'brightness of the first' /run/camlive_$T.out | head -2 | cut -c20-120
sleep 3; systemctl restart q2-headtrack
