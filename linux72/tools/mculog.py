#!/usr/bin/python3
# mculog: passive reader of the SyncBoss control fifo (MCU log records 0x92, assertions 0xd8). Read-only, never writes.
import os, sys, time, select
ctl = os.open('/dev/syncboss_control0', os.O_RDONLY | os.O_NONBLOCK); buf = b''; end = time.time() + float(sys.argv[1])
t0 = time.time()
while time.time() < end:
    r, _, _ = select.select([ctl], [], [], 0.2)
    if not r: continue
    try: buf += os.read(ctl, 65536)
    except BlockingIOError: continue
    while len(buf) >= 3:
        ty, sq, l = buf[0], buf[1], buf[2]
        if len(buf) < 3 + l: break
        d = buf[3:3 + l]; buf = buf[3 + l:]
        if ty in (0x92, 0xd8):
            txt = ''.join(chr(c) if 32 <= c < 127 else '' for c in d)
            print('%5.1fs 0x%02x %s' % (time.time() - t0, ty, txt[:150]), flush=True)
        elif ty not in (0x01, 0x7d): print('%5.1fs 0x%02x len=%d %s' % (time.time() - t0, ty, l, d[:8].hex()), flush=True)
