#!/usr/bin/python3
# ledscan2.py - for each LED offset capture one frame per camera with the LED pulse OFF (on-time 1 us) and ON, into /run/scan2 as raw files (analysis on the host).
import mmap, os, struct, subprocess, sys, time
PERIOD = int(sys.argv[1]); ONTIME = int(sys.argv[2]); STEP = int(sys.argv[3])
f = open('/dev/shm/q2cam', 'rb'); m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
magic, ver, ncam, W, H, NS, HS, ST = struct.unpack_from('<8I', m, 0)
os.makedirs('/run/scan2', exist_ok=True)
for fn in os.listdir('/run/scan2'): os.unlink('/run/scan2/' + fn)
def led(o, off): subprocess.run(['/usr/bin/q2ledctl', 'set', 'both', str(PERIOD), str(o), str(off)], stdout=subprocess.DEVNULL, check=True)
def grab(tag):
    time.sleep(0.45)
    for c in range(ncam):
        pub, = struct.unpack_from('<Q', m, 0x40 + c * 64); n = pub - 1
        off = HS + (c * NS + n % NS) * ST
        open('/run/scan2/%s_c%d.raw' % (tag, c), 'wb').write(bytes(m[off + 64: off + 64 + W * H]))
for off in range(0, PERIOD, STEP):
    led(1, off); grab('off_%05d' % off)
    led(ONTIME, off); grab('on_%05d' % off)
led(34, 29830) if False else subprocess.run(['/usr/bin/q2ledctl', 'set', 'both', '39996', '34', '29830'], stdout=subprocess.DEVNULL)
print('done', len(os.listdir('/run/scan2')), 'files')
