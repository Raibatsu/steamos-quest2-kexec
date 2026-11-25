import mmap, struct, os
f = open('/dev/shm/q2cam', 'rb'); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
magic, ver, ncam, W, H, NS, HS, ST = struct.unpack_from('<8I', m, 0)
os.makedirs('/run/ring', exist_ok=True)
for c in range(ncam):
    pub, = struct.unpack_from('<Q', m, 0x40 + c * 64)
    out = bytearray()
    for n in range(max(0, pub - NS), pub):
        off = HS + (c * NS + n % NS) * ST
        s0, = struct.unpack_from('<Q', m, off)
        out += struct.pack('<QQ', n, s0) + bytes(m[off + 64: off + 64 + W * H])
    open('/run/ring/cam%d.bin' % c, 'wb').write(out)
print('NS', NS, 'W', W, 'H', H, 'ncam', ncam)
