#!/usr/bin/python3
# sbprobe: read-mostly SyncBoss controller-radio probe. Run with q2-headtrack and SteamVR stopped.
# Frames are {type, seq, len, data...}. GET_DATA=2, SET_DATA=3 (vendor driver). Never sends pair/unpair (0xd1).
import os, sys, time, select, struct

C = '/sys/devices/virtual/misc/syncboss0/spi/control/'
def wr(p, v):
    try:
        with open(p, 'w') as f: f.write(str(v))
    except Exception as e: print('warn', p, e)
wr('/sys/class/firmware/timeout', 1)
wr(C + 'transaction_length', 248); wr(C + 'transaction_period_us', 1000); wr(C + 'minimum_time_between_transactions_us', 500)

cmd = os.open('/dev/syncboss0', os.O_RDWR)
st = os.open('/dev/syncboss_stream0', os.O_RDONLY)
buf = b''
seen = {}
def pump(dur, show=True, hide=(0x50, 0x01)):
    global buf
    end = time.time() + dur
    out = []
    while time.time() < end:
        r, _, _ = select.select([st], [], [], 0.1)
        if not r: continue
        buf += os.read(st, 65536)
        while len(buf) >= 3:
            ty, sq, l = buf[0], buf[1], buf[2]
            if len(buf) < 3 + l: break
            d = buf[3:3 + l]; buf = buf[3 + l:]
            seen[ty] = seen.get(ty, 0) + 1
            if ty in hide: continue
            out.append((ty, sq, d))
            if show: print('  rec type=0x%02x seq=%d len=%d data=%s' % (ty, sq, l, d[:48].hex()))
    return out

def send(ty, seq, data=b''):
    os.write(cmd, bytes([ty, seq, len(data)]) + data)

def get(reg, seq):
    print('GET_DATA 0x%02x' % reg)
    send(2, seq, bytes([reg]))
    return pump(0.6)

time.sleep(1.0)
send(0x6e, 1)          # IMU enable: wakes the stream like the tracker does
print('IMU enable sent'); pump(0.5, show=False)
print('== initial state'); get(0xd2, 10); get(0x7c, 11)
print('== SET_DATA 0xd5 = 0 (auto-whitelisting off, as the vendor input_start does)')
send(3, 12, bytes([0xd5, 0])); pump(0.4)
print('== beacon mode (cmd 0x85)')
send(0x85, 13); pump(0.6)
print('== state after beacon'); get(0xd2, 14); get(0x7c, 15)
print('== PRESS CONTROLLER BUTTONS NOW (20 s): non-IMU records follow')
t0 = time.time()
while time.time() - t0 < 20:
    pump(2.0)
    if int(time.time() - t0) % 10 == 0: send(0x85, 16)
print('record type counts:', ' '.join('%02x:%d' % kv for kv in sorted(seen.items())))
