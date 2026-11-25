#!/usr/bin/python3
# q2stockmount: mount the stock (Meta) partitions of the Quest 2's super partition READ-ONLY for inspection.
# dm tables are created with --readonly, filesystems mounted ro,noload, nothing is ever written to the device.
# A small margin (1 MiB of the following sectors) is mapped after the last extent: some partitions' ext4 block count is
# a few blocks larger than the extents in the LP metadata, and ext4 refuses the mount otherwise.
# usage: q2stockmount.py mount | umount
import os, struct, subprocess, sys
SUP = '/dev/disk/by-partlabel/super'; BASE = '/run/q2stock'; WANT = ('vendor_a', 'odm_a', 'product_a', 'system_ext_a', 'system_a')
def sh(c): r = subprocess.run(c, shell=True, capture_output=True, text=True); return r.returncode, (r.stdout + r.stderr).strip()
def parts():
    f = open(SUP, 'rb'); f.seek(4096 + 8192); hdr = f.read(256)
    hmagic, major, minor, hsize = struct.unpack_from('<IHHI', hdr, 0)
    assert hmagic == 0x414C5030, 'no LP metadata'
    o = 4 + 2 + 2 + 4 + 32; ts, = struct.unpack_from('<I', hdr, o); o += 4 + 32
    desc = [struct.unpack_from('<III', hdr, o + 12 * i) for i in range(4)]
    f.seek(4096 + 8192 + hsize); t = f.read(ts); (po, pn, ps), (eo, en, es) = desc[0], desc[1]
    ex = [struct.unpack_from('<QIQI', t, eo + i * es) for i in range(en)]; out = {}
    for i in range(pn):
        b = t[po + i * ps: po + (i + 1) * ps]; name = b[:36].split(b'\0')[0].decode()
        a, first, num, g = struct.unpack_from('<IIII', b, 36); out[name] = [ex[first + k] for k in range(num)]
    return out
if sys.argv[1] == 'mount':
    sup = os.path.realpath(SUP); P = parts()
    for name in WANT:
        e = P.get(name)
        if not e: print(name, 'missing'); continue
        tab = []; s = 0
        for (n, tt, td, ts_) in e:
            assert tt == 0 and ts_ == 0, 'non-linear extent'
            tab.append('%d %d linear %s %d' % (s, n, sup, td)); s += n
        tab.append('%d 2048 linear %s %d' % (s, sup, e[-1][2] + e[-1][0]))      # 1 MiB margin after the last extent
        open('/run/%s.table' % name, 'w').write('\n'.join(tab) + '\n')
        dm = 'q2ro_' + name; sh('umount %s/%s 2>/dev/null; dmsetup remove %s 2>/dev/null' % (BASE, name, dm))
        rc, out = sh('dmsetup create --readonly %s /run/%s.table' % (dm, name))
        if rc: print(name, 'dmsetup failed:', out); continue
        mp = '%s/%s' % (BASE, name); os.makedirs(mp, exist_ok=True)
        rc, out = sh('mount -t ext4 -o ro,noload /dev/mapper/%s %s' % (dm, mp)); print(name, 'mounted ro at', mp if rc == 0 else 'FAILED: ' + out[-120:])
else:
    for name in WANT:
        sh('umount %s/%s 2>/dev/null; dmsetup remove q2ro_%s 2>/dev/null' % (BASE, name, name)); print(name, 'released')
