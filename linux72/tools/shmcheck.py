import struct, time, mmap, os
def rd():
    f = open('/dev/shm/q2cam', 'rb'); m = mmap.mmap(f.fileno(), 4096, prot=mmap.PROT_READ)
    hdr = struct.unpack_from('<8I', m, 0)
    pub = [struct.unpack_from('<Q', m, 0x40 + c * 64)[0] for c in range(4)]
    m.close(); f.close(); return hdr, pub
h, a = rd(); time.sleep(2.0); h, b = rd()
print('magic %#x ncams %d %dx%d' % (h[0], h[2], h[3], h[4]))
print('published now:', b, ' fps per camera over 2 s:', [round((y - x) / 2.0, 1) for x, y in zip(a, b)])
