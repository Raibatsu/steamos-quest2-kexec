#!/usr/bin/python3
import mmap, os, struct, sys, time
sys.argv = [sys.argv[0]] + sys.argv[1:]
exec(open('/usr/bin/maskprobe.py').read().split("a = sys.argv[1:]")[0])
def stat():
    f = open('/dev/shm/q2cam', 'rb'); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    magic, ver, ncam, W, H, NS, HS, ST = struct.unpack_from('<8I', m, 0); out = []
    for c in range(ncam):
        pub, = struct.unpack_from('<Q', m, 0x40 + c * 64); n = pub - 1
        off = HS + (c * NS + n % NS) * ST; px = bytes(m[off + 64: off + 64 + W * H])
        out.append('cam%d mean %5.1f sat %4d >=128 %4d >=64 %4d' % (c, sum(px[::13]) / len(px[::13]), px.count(255), sum(1 for v in px[::2] if v >= 128) * 2, sum(1 for v in px[::2] if v >= 64) * 2))
    return out
for e, g in ((5000, 48), (3000, 32), (1500, 16), (600, 16), (200, 16)):
    setgrp((1, 8), e, g); setgrp((4, 2), e, g); time.sleep(1.2)
    print('exp %5d gain %3d' % (e, g)); [print('   ' + l) for l in stat()]
