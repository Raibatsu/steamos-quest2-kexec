#!/usr/bin/python3
# hzmode.py - send Horizon's camera configuration (strace of Horizon, 2026-11-26) through the tracker's sbcam trigger, then measure brightness.
import os, struct, sys, time
sys.path.insert(0, '/usr/bin')
def send(lines):
    open('/dev/shm/q2ht-sbcam.tmp', 'w').write('\n'.join(lines) + '\n'); os.rename('/dev/shm/q2ht-sbcam.tmp', '/dev/shm/q2ht-sbcam')
    for _ in range(80):
        if not os.path.exists('/dev/shm/q2ht-sbcam'): return
        time.sleep(0.05)
def msg(ty, seq, data): return (bytes([ty, seq, len(data)]) + data).hex()
ef = bytes([0xef]) + b''.join(struct.pack('<IIBII', cid, 0, 0, 5000, 48) for cid in (0x10, 0x11, 0x12, 0x13))
a3 = bytes([0xa3]) + struct.pack('<II', 0x0f, 20000)
which = sys.argv[1] if len(sys.argv) > 1 else 'all'
if which in ('ef', 'all'): send([msg(3, 0x70, ef), msg(3, 0x71, bytes([0x64]))])
if which in ('a3', 'all'): send([msg(3, 0x72, a3), msg(3, 0x73, bytes([0x64]))])
print('sent', which)
