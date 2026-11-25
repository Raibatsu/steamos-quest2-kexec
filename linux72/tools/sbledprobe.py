#!/usr/bin/python3
# sbledprobe: READ a Touch controller's IR-LED sync register (controller register 0x28) through SyncBoss. Read-only: it never writes to a controller.
# Run on the headset with q2-headtrack stopped (it owns /dev/syncboss0), controllers awake and connected (press a button on each):
#     systemctl stop q2-camd q2-ctltrack q2-headtrack; python3 sbledprobe.py; systemctl start q2-headtrack
# Decoded from Meta's libsyncboss (2026-11-26): a controller register request is a SET_DATA/GET_DATA message on MCU register 0x8f (0xe2 for other address types) with a 20-byte header
#   [u64 controller id][8 zero bytes][u16 timeout ms (2400)][1 pad][u8 controller register]  + data bytes; the answer arrives on the data stream as a 0x8f notification of the same layout
#   (the tracker's own notifications have the register at byte 19: 0/1 for the regular reports).
# Register 0x28 = LED sync trigger {u32 period_us (global default 33333), u32 on_time_us (per controller, set by the host), s32 offset_us (global default -9)}.
import os, sys, time, select, struct
C = '/sys/devices/virtual/misc/syncboss0/spi/control/'
def wr(p, v):
    try:
        with open(p, 'w') as f: f.write(str(v))
    except Exception: pass
wr(C + 'transaction_length', 248); wr(C + 'transaction_period_us', 1000); wr(C + 'minimum_time_between_transactions_us', 500)
cmd = os.open('/dev/syncboss0', os.O_RDWR)
st = os.open('/dev/syncboss_stream0', os.O_RDONLY | os.O_NONBLOCK)
ctl = os.open('/dev/syncboss_control0', os.O_RDONLY | os.O_NONBLOCK)
bufs = {'S': b'', 'C': b''}; seq = [0]; IDS = {}
def pump(dur, show=None):
    end = time.time() + dur; out = []
    while time.time() < end:
        r, _, _ = select.select([st, ctl], [], [], 0.02)
        for fd, tag in ((st, 'S'), (ctl, 'C')):
            if fd not in r: continue
            try: chunk = os.read(fd, 65536)
            except BlockingIOError: continue
            bufs[tag] += chunk; b = bufs[tag]
            while len(b) >= 3:
                ty, sq, l = b[0], b[1], b[2]
                if len(b) < 3 + l: break
                d = b[3:3 + l]; b = b[3 + l:]
                if ty == 0x8f and len(d) >= 24:
                    cid, ptype = struct.unpack_from('<QI', d, 0)
                    IDS[cid] = ptype
                    if d[19] not in (0, 1): out.append((tag, ty, d))        # a register answer, not a regular report
                elif ty in (0x92, 0xd8, 0x7d) or (tag == 'C' and ty not in (0x01,)): out.append((tag, ty, d))
            bufs[tag] = b
    return out
def send(ty, data):
    seq[0] = (seq[0] + 1) & 0xff; os.write(cmd, bytes([ty, seq[0], len(data)]) + data)
def req_header(cid, reg): return struct.pack('<Q8xHxB', cid, 2400, reg)          # 20 bytes
def show(recs):
    for tag, ty, d in recs:
        txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in d)
        print('   [%s] type=0x%02x len=%d %s | %s' % (tag, ty, len(d), d[:48].hex(), txt[:60]), flush=True)

os.write(cmd, bytes([0x6e, 1, 0])); pump(1.0)
send(0x85, bytes([1, 1])); pump(0.5)                                              # radio beacon mode, as the tracker does
print('listening for controllers (press a button on each)...', flush=True)
pump(6.0)
for cid, pt in sorted(IDS.items()):
    print('controller id %016x pulsar_type %#010x side code %d' % (cid, pt, pt >> 16 & 0xff))
if not IDS: sys.exit('no controllers seen')
REG = 0x28
WRITE = None
if len(sys.argv) >= 5 and sys.argv[1] == '--write':           # --write <period_us> <on_time_us> <offset_us>  : only controller register 0x28, bounds-checked
    p, o, f = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    assert 1000 <= p <= 200000 and 1 <= o <= 2000 and abs(f) < p, 'values out of the allowed range'
    WRITE = struct.pack('<IIi', p, o, f)
for cid in sorted(IDS):
    print('%016x: GET reg 0x28' % cid, flush=True); send(2, bytes([0x8f]) + req_header(cid, REG)); recs = pump(1.2); show(recs)
    for tag, ty, d in recs:
        if ty == 0x8f and len(d) >= 32 and d[19] == REG: print('   -> period %d us, on-time %d us, offset %d us' % struct.unpack_from('<IIi', d, 20))
if WRITE:
    for cid in sorted(IDS):
        print('%016x: WRITE reg 0x28 = %s' % (cid, ' '.join(str(x) for x in struct.unpack('<IIi', WRITE))), flush=True)
        send(3, bytes([0x8f]) + req_header(cid, REG) + WRITE); show(pump(1.5))
        print('%016x: read back' % cid, flush=True); send(2, bytes([0x8f]) + req_header(cid, REG)); recs = pump(1.2); show(recs)
        for tag, ty, d in recs:
            if ty == 0x8f and len(d) >= 32 and d[19] == REG: print('   -> now period %d us, on-time %d us, offset %d us' % struct.unpack_from('<IIi', d, 20))
else:
    print('done (nothing was written to any controller)')
