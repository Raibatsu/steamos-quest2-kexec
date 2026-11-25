import mmap, struct, sys, os
f = open('/dev/shm/q2cam', 'rb'); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
magic, ver, ncam, W, H, NS, HS, ST = struct.unpack_from('<8I', m, 0)
os.makedirs('/run/fd', exist_ok=True)
for c in range(ncam):
    pub, = struct.unpack_from('<Q', m, 0x40 + c * 64)
    n = pub - 1; off = HS + (c * NS + n % NS) * ST
    px = bytes(m[off + 64: off + 64 + W * H])
    open('/run/fd/cam%d.pgm' % c, 'wb').write(b'P5 %d %d 255\n' % (W, H) + px)
print('dumped', ncam)
