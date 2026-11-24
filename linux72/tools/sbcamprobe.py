#!/usr/bin/python3
# sbcamprobe: send the same five whitelisted camera commands q2camlive uses (stop, probe, two probe steps, start) and
# print everything the MCU says back, including its log records (type 0x92). Run with q2-headtrack stopped.
# Never sends pair/unpair (0xd1).
import os, sys, time, select
C = '/sys/devices/virtual/misc/syncboss0/spi/control/'
def wr(p, v):
    try:
        with open(p, 'w') as f: f.write(str(v))
    except Exception as e: print('warn', p, e)
wr('/sys/class/firmware/timeout', 1)
wr(C + 'transaction_length', 248); wr(C + 'transaction_period_us', 1000); wr(C + 'minimum_time_between_transactions_us', 500)
cmd = os.open('/dev/syncboss0', os.O_RDWR)
st = os.open('/dev/syncboss_stream0', os.O_RDONLY | os.O_NONBLOCK)
ctl = os.open('/dev/syncboss_control0', os.O_RDONLY | os.O_NONBLOCK)
bufs = {'S': b'', 'C': b''}; seen = {}
def pump(dur, label=''):
    end = time.time() + dur
    while time.time() < end:
        r, _, _ = select.select([st, ctl], [], [], 0.05)
        for fd, tag in ((st, 'S'), (ctl, 'C')):
            if fd not in r: continue
            try: chunk = os.read(fd, 65536)
            except BlockingIOError: continue
            bufs[tag] += chunk; b = bufs[tag]
            while len(b) >= 3:
                ty, sq, l = b[0], b[1], b[2]
                if len(b) < 3 + l: break
                d = b[3:3 + l]; b = b[3 + l:]
                seen[(tag, ty)] = seen.get((tag, ty), 0) + 1
                if tag == 'S' and ty in (0x01, 0x50, 0x8f): continue
                txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in d)
                print('  %s [%s] type=0x%02x seq=%d len=%d %s | %s' % (label, tag, ty, sq, l, d[:4].hex(), txt[:120]))
            bufs[tag] = b
def send(hexs):
    b = bytes.fromhex(hexs)
    assert b[0] in (0x6e, 0x2a, 0x2c, 0x2d, 0x28) or (b[0] == 0x03 and b[3] == 0xdd), 'not whitelisted'
    assert len(b) == 3 + b[2]
    os.write(cmd, b); print('sent', hexs)
time.sleep(1.0)
send('6e0000'); pump(1.0, 'imu-enable')
import sys
for h in sys.argv[1:]:
    send(h); pump(1.2, h[:8])
print('record counts:', ' '.join('%s%02x:%d' % (k[0], k[1], v) for k, v in sorted(seen.items())))
