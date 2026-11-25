#!/usr/bin/python3
# maskprobe.py: set exposure/gain per a1 mask pair and report brightness per frame phase (seq % 4) of camera 0..3.
import mmap, os, struct, sys, time
SB = '/dev/shm/q2ht-sbcam'
def send(lines):
    for _ in range(100):
        if not os.path.exists(SB): break
        time.sleep(0.05)
    open(SB + '.t', 'w').write('\n'.join(lines) + '\n'); os.rename(SB + '.t', SB)
def msg(data, seq): return (bytes([3, seq & 0xff, len(data)]) + data).hex()
def rec(mask, cid, e, g):
    r = bytearray(105); struct.pack_into('<IIHH', r, 0, mask, cid, e, g); struct.pack_into('<I', r, 20, 1); return bytes(r)
def setgrp(masks, e, g, ids=(0x10, 0x11, 0x12, 0x13)):
    lines = []
    for cid in ids:
        lines.append(msg(bytes([0xa1]) + rec(masks[0], cid, e, g) + rec(masks[1], cid, e, g), 0x90 + len(lines)))
    for i in range(0, len(lines), 8): send(lines[i:i + 8])
def phases():
    f = open('/dev/shm/q2cam', 'rb'); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    magic, ver, ncam, W, H, NS, HS, ST = struct.unpack_from('<8I', m, 0)
    res = {}
    for c in range(ncam):
        pub, = struct.unpack_from('<Q', m, 0x40 + c * 64)
        for n in range(pub - NS, pub):
            off = HS + (c * NS + n % NS) * ST
            px = m[off + 64: off + 64 + W * H]
            res.setdefault(c, {}).setdefault(n % 4, []).append(sum(px[::13]) / len(px[::13]))
    return res
def show(tag):
    r = phases(); print(tag)
    for c in sorted(r): print('  cam%d  phase means: %s' % (c, ' '.join('p%d=%5.1f' % (p, sum(v) / len(v)) for p, v in sorted(r[c].items()))))
a = sys.argv[1:]
# usage: maskprobe.py M1 M2 EXP GAIN  -> set that mask pair
if a: setgrp((int(a[0]), int(a[1])), int(a[2]), int(a[3])); time.sleep(1.2)
show('state')
