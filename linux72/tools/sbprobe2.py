#!/usr/bin/python3
# sbprobe2: find the payload that puts the SyncBoss radio into beacon mode (state 0xd2 == 2). Never pairs/unpairs.
import os, sys, time, select
C = '/sys/devices/virtual/misc/syncboss0/spi/control/'
for p, v in (('transaction_length', 248), ('transaction_period_us', 1000), ('minimum_time_between_transactions_us', 500)):
    try: open(C + p, 'w').write(str(v))
    except Exception as e: print('warn', p, e)
cmd = os.open('/dev/syncboss0', os.O_RDWR)
st = os.open('/dev/syncboss_stream0', os.O_RDONLY | os.O_NONBLOCK)
ctl = os.open('/dev/syncboss_control0', os.O_RDONLY | os.O_NONBLOCK)
bufs = {st: b'', ctl: b''}
seen = {}
def pump(dur, quiet=True):
    out = []; end = time.time() + dur
    while time.time() < end:
        r, _, _ = select.select([st, ctl], [], [], 0.1)
        for fd in r:
            try: bufs[fd] += os.read(fd, 65536)
            except BlockingIOError: continue
            b = bufs[fd]
            while len(b) >= 3:
                ty, sq, l = b[0], b[1], b[2]
                if len(b) < 3 + l: break
                d = b[3:3 + l]; b = b[3 + l:]
                tag = 'S' if fd == st else 'C'
                seen[(tag, ty)] = seen.get((tag, ty), 0) + 1
                if tag == 'S' and ty in (0x50, 0x01): continue
                out.append((tag, ty, sq, d))
            bufs[fd] = b
    return out
def show(recs):
    for tag, ty, sq, d in recs:
        if tag == 'C' and ty == 0x01: continue
        if ty == 0x92:
            txt = d[3:].split(b'\0')[0].decode('ascii', 'replace') if len(d) > 3 else ''
            print('    MCU-LOG:', txt.strip()[:160])
        else:
            print('    [%s] type=0x%02x seq=%d len=%d data=%s' % (tag, ty, sq, len(d), d[:40].hex()))
def send(ty, seq, data=b''): os.write(cmd, bytes([ty, seq, len(data)]) + data)
def state(seq):
    send(2, seq, bytes([0xd2])); recs = pump(0.4)
    for tag, ty, sq, d in recs:
        if ty == 0xd2 and sq == seq: return d[0] if d else None
    return None
time.sleep(1.0); send(0x6e, 1); pump(0.5)
print('initial 0xd2 state =', state(10))
seq = 20
for pl in (b'', b'\x01', b'\x02', b'\x03', b'\x01\x00', b'\x01\x01', b'\x00\x01', b'\x01\x00\x00\x00', b'\x02\x00\x00\x00'):
    seq += 1
    print('--- 0x85 payload', pl.hex() or '(empty)')
    send(0x85, seq, pl); show(pump(0.6))
    s = state(seq + 100)
    print('    state after =', s)
    if s == 2:
        print('*** BEACON MODE ON with payload', pl.hex() or '(empty)')
        break
print('== listening 20 s for controller packets (press buttons on both controllers)')
t0 = time.time(); n8f = 0
while time.time() - t0 < 20:
    for tag, ty, sq, d in pump(1.0):
        if tag == 'S' and ty in (0x8f, 0x90, 0x93):
            n8f += 1
            if n8f <= 12: print('  CTL pkt type=0x%02x len=%d %s' % (ty, len(d), d[:40].hex()))
        elif ty == 0x92:
            show([(tag, ty, sq, d)])
print('controller packets:', n8f, '| counts:', ' '.join('%s%02x:%d' % (k[0], k[1], v) for k, v in sorted(seen.items())))
