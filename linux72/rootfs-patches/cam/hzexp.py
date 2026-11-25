#!/usr/bin/python3
# hzexp.py - try Horizon's per-camera exposure/gain message (MCU register 0xa1) and measure frame brightness in /dev/shm/q2cam.
#   hzexp.py measure                       brightness of the newest frame of each camera
#   hzexp.py set EXPOSURE GAIN [ids...]   send 0xa1 to the cameras (default ids 0x10 0x11 0x12 0x13), then measure
import mmap, os, struct, sys, time
def measure():
    f = open('/dev/shm/q2cam', 'rb'); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    magic, ver, ncam, W, H, NS, HS, ST = struct.unpack_from('<8I', m, 0)
    out = []
    for c in range(ncam):
        pub, = struct.unpack_from('<Q', m, 0x40 + c * 64)
        if not pub: out.append((c, None)); continue
        n = pub - 1; off = HS + (c * NS + n % NS) * ST
        px = m[off + 64: off + 64 + W * H]
        hist = [0] * 256
        for v in px[::7]: hist[v] += 1
        tot = sum(hist); acc = 0; p99 = 0
        for i, h in enumerate(hist):
            acc += h
            if acc >= 0.99 * tot: p99 = i; break
        out.append((c, (max(px), p99, sum(px[::7]) / max(1, len(px[::7])))))
    return out
def show(tag):
    print(tag, ' '.join('cam%d:%s' % (c, ('max %3d p99 %3d mean %5.1f' % v) if v else 'no frame') for c, v in measure()), flush=True)
def a1(cid, exp, gain, g1, g2):
    def rec(mask):
        r = bytearray(105); struct.pack_into('<IIHH', r, 0, mask, cid, exp, gain); struct.pack_into('<I', r, 20, 1); return bytes(r)
    d = bytes([0xa1]) + rec(g1) + rec(g2)
    b = bytes([3, 0x50, len(d)]) + d
    open('/dev/shm/q2ht-sbcam.tmp', 'w').write(b.hex() + '\n'); os.rename('/dev/shm/q2ht-sbcam.tmp', '/dev/shm/q2ht-sbcam')
    for _ in range(40):
        if not os.path.exists('/dev/shm/q2ht-sbcam'): break
        time.sleep(0.05)
if sys.argv[1] == 'measure': show('now')
elif sys.argv[1] == 'set':
    exp, gain = int(sys.argv[2]), int(sys.argv[3]); ids = [int(x, 0) for x in sys.argv[4:]] or [0x10, 0x11, 0x12, 0x13]
    show('before')
    for rep in range(4):                      # Horizon repeats the update ~25 times a second
        for cid in ids:
            for g1, g2 in ((1, 8), (4, 2)): a1(cid, exp, gain, g1, g2)
        time.sleep(0.2)
    time.sleep(0.8); show('after ')
