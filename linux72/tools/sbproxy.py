#!/usr/bin/python3
# sbproxy: stand-in for q2headtrack's camera-command forwarder, with the MCU's replies printed. Run with q2-headtrack
# stopped. Same whitelist as the tracker (0x2a 0x2c 0x2d 0x28 and 0x03 .. 0xdd), root-owned request file only. Never 0xd1.
import os, sys, time, select
C = '/sys/devices/virtual/misc/syncboss0/spi/control/'
def wr(p, v):
    try:
        with open(p, 'w') as f: f.write(str(v))
    except Exception: pass
wr(C + 'transaction_length', 248); wr(C + 'transaction_period_us', 1000); wr(C + 'minimum_time_between_transactions_us', 500)
cmd = os.open('/dev/syncboss0', os.O_RDWR)
st = os.open('/dev/syncboss_stream0', os.O_RDONLY | os.O_NONBLOCK)
ctl = os.open('/dev/syncboss_control0', os.O_RDONLY | os.O_NONBLOCK)
open('/run/q2headtrack.pid', 'w').write('%d\n' % os.getpid())
bufs = {'S': b'', 'C': b''}; seen = {}
def pump(dur):
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
                if ty in (0x01, 0x50, 0x8f, 0x7d): continue
                txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in d)
                print('   MCU [%s] type=0x%02x len=%d %s | %s' % (tag, ty, l, d[:8].hex(), txt[:130]), flush=True)
            bufs[tag] = b
os.write(cmd, bytes([0x6e, 1, 0])); pump(1.0)
T = '/dev/shm/q2ht-sbcam'; t_end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 40)
print('proxy ready', flush=True)
while time.time() < t_end:
    pump(0.1)
    if os.path.exists(T):
        try: stt = os.stat(T); lines = open(T).read().split()
        except Exception: stt = None; lines = []
        try: os.unlink(T)
        except Exception: pass
        if stt is not None and stt.st_uid == 0:
            for ln in lines[:8]:
                b = bytes.fromhex(ln)
                ok = len(b) >= 3 and len(b) == 3 + b[2] and (b[0] in (0x2a, 0x2c, 0x2d, 0x28) or (b[0] == 0x03 and len(b) >= 4 and b[3] == 0xdd))
                if not ok: print('refused', ln, flush=True); continue
                os.write(cmd, b); print('sent', b.hex(), flush=True); pump(0.8)
print('record counts:', ' '.join('%s%02x:%d' % (k[0], k[1], v) for k, v in sorted(seen.items())))
try: os.unlink('/run/q2headtrack.pid')
except Exception: pass
