#!/usr/bin/env python3
"""Build the DTB + pick the physical base for a Linux->Linux kexec with claimed RAM (q2kexec -B).

usage: make_q2kexec_dtb.py <live.dtb> <kexec.dtb> <Image> <initrd> <out.dtb>
  live.dtb  = /sys/firmware/fdt of the RUNNING kernel (its memory node holds the ranges Horizon gave us)
  kexec.dtb = the compiled sm8250-oculus-hollywood-kexec.dtb to boot next
The DTB gets the live memory node and initrd props matching q2kexec's layout (kernel, initrd, dtb, trampoline back to back);
prints the base to pass as `q2kexec -B <base> -D <out.dtb> <Image> <initrd>`. Fails if no claimed range holds the layout.
"""
import os, struct, subprocess, sys

live, kdtb, img, rd, out = sys.argv[1:6]
raw = subprocess.run(["fdtget", "-tx", live, "/memory@80000000", "reg"], capture_output=True, text=True, check=True).stdout.split()
ranges = []
for i in range(0, len(raw), 4):
    a = int(raw[i], 16) << 32 | int(raw[i + 1], 16)
    s = int(raw[i + 2], 16) << 32 | int(raw[i + 3], 16)
    if s:
        ranges.append((a, a + s))
hdr = open(img, "rb").read(64)
off, size = struct.unpack_from("<QQ", hdr, 8)
memsz = (max(size, os.path.getsize(img)) + 4095) & ~4095
rsz = os.path.getsize(rd)
imem = (rsz + 4095) & ~4095
dsz = (os.path.getsize(kdtb) + 4095) & ~4095  # live memory node has the same entry count as the placeholder: size unchanged

def layout(base):
    kp = base + off
    ip = kp + memsz
    dp = (ip + imem + 0x1fffff) & ~0x1fffff
    return kp, ip, dp, dp + dsz + 4096

for a, b in sorted(ranges, key=lambda r: r[1] - r[0], reverse=True):
    base = (a + 0x1fffff) & ~0x1fffff
    kp, ip, dp, end = layout(base)
    if end <= b:
        break
else:
    sys.exit("no claimed range is large enough for kernel+initrd+dtb")

subprocess.run(["cp", kdtb, out], check=True)
subprocess.run(["fdtput", "-t", "x", out, "/memory@80000000", "reg", *raw], check=True)
ie = ip + rsz
subprocess.run(["fdtput", "-t", "u", out, "/chosen", "linux,initrd-start", str(ip >> 32), str(ip & 0xffffffff)], check=True)
subprocess.run(["fdtput", "-t", "u", out, "/chosen", "linux,initrd-end", str(ie >> 32), str(ie & 0xffffffff)], check=True)
print("%x" % base)
