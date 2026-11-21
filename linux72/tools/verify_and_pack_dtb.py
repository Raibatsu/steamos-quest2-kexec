#!/usr/bin/env python3
"""Verify Linux 7.2 arm64 Image + stage1 initramfs + DTB against loader/loader.c."""

import os
import struct
import subprocess
import sys

PAGE_SIZE = 4096
SZ_2M = 2 * 1024 * 1024
SZ_64M = 0x4000000


def page_align(x: int) -> int:
    return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)


def align_up(x: int, a: int) -> int:
    return (x + a - 1) & ~(a - 1)


def qkx_check_dtb(b: bytes, payload_initrd: int, rsize: int) -> None:
    dsize = len(b)
    if dsize < 40 or dsize > SZ_2M:
        raise ValueError(f"bad dtb size {dsize}")
    magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap, version, last_comp_version, _, size_dt_strings, size_dt_struct = struct.unpack_from(">10I", b, 0)
    if magic != 0xD00DFEED:
        raise ValueError(f"bad fdt magic {magic:#x}")
    if totalsize != dsize:
        raise ValueError(f"totalsize {totalsize} != dsize {dsize}")
    if version != 17 or last_comp_version > 17:
        raise ValueError(f"bad fdt version {version}/{last_comp_version}")
    pos = off_dt_struct
    strings = off_dt_strings
    reserve = off_mem_rsvmap
    slen = size_dt_strings
    if reserve < 40 or (reserve % 8) != 0 or reserve >= pos or (pos % 4) != 0 or pos > strings or strings > dsize or slen > dsize - strings or size_dt_struct > strings - pos:
        raise ValueError("bad fdt offsets/bounds")
    end = pos + size_dt_struct

    terminated = False
    while reserve + 16 <= pos:
        addr, length = struct.unpack_from(">2Q", b, reserve)
        reserve += 16
        if addr == 0 and length == 0:
            terminated = True
            break
        if length == 0 or addr > (1 << 64) - 1 - length:
            raise ValueError("bad mem_rsvmap entry")
    if not terminated:
        raise ValueError("unterminated mem_rsvmap")

    depth = 0
    seen = 0
    root = False
    chosen = False
    chosen_seen = False
    children = [False] * 64

    while pos + 4 <= end:
        token = struct.unpack_from(">I", b, pos)[0]
        pos += 4
        if token == 1:  # FDT_BEGIN_NODE
            name_start = pos
            while pos < end and b[pos] != 0:
                pos += 1
            if pos == end or depth >= 64 or (depth == 0 and (root or pos != name_start)):
                raise ValueError("bad BEGIN_NODE")
            if depth:
                children[depth - 1] = True
            children[depth] = False
            if depth == 0:
                root = True
            node_name = b[name_start:pos]
            if depth == 1 and node_name in (b"chosen", b"chosen@0"):
                if chosen_seen:
                    raise ValueError("duplicate chosen node")
                chosen_seen = True
                chosen = True
            depth += 1
            pos = align_up(pos + 1, 4)
        elif token == 2:  # FDT_END_NODE
            if depth == 0:
                raise ValueError("unexpected END_NODE")
            if depth == 2:
                chosen = False
            depth -= 1
        elif token == 3:  # FDT_PROP
            if depth == 0 or children[depth - 1] or end - pos < 8:
                raise ValueError(f"bad FDT_PROP placement/bounds (depth={depth}, children_prev={children[depth-1] if depth else None})")
            plen, pname = struct.unpack_from(">2I", b, pos)
            pos += 8
            if plen > end - pos or pname >= slen:
                raise ValueError("bad FDT_PROP length/name")
            stop = pname
            while stop < slen and b[strings + stop] != 0:
                stop += 1
            if stop == slen:
                raise ValueError("unterminated prop name")
            prop_name = b[strings + pname:strings + stop]
            bit = 0
            if chosen and depth == 2:
                if prop_name == b"linux,initrd-start":
                    bit = 1
                elif prop_name == b"linux,initrd-end":
                    bit = 2
            if bit:
                if (seen & bit) or plen not in (4, 8):
                    raise ValueError(f"bad initrd prop len={plen} seen={seen}")
                val = struct.unpack_from(">Q" if plen == 8 else ">I", b, pos)[0]
                expected = payload_initrd + (rsize if bit == 2 else 0)
                if val != expected:
                    raise ValueError(f"initrd prop {prop_name.decode()}={val:#x} != expected {expected:#x}")
                seen |= bit
            pos += align_up(plen, 4)
        elif token == 9:  # FDT_END
            if not (root and depth == 0 and seen == 3 and pos == end):
                raise ValueError(f"bad FDT_END state: root={root} depth={depth} seen={seen} pos={pos} end={end}")
            return
        elif token != 4:  # FDT_NOP
            raise ValueError(f"bad FDT token {token}")
    raise ValueError("missing FDT_END")


def main() -> int:
    out_dir = "/mnt/HDD/quest2-linux72/out"
    image_path = os.path.join(out_dir, "arch/arm64/boot/Image")
    initrd_path = os.path.join(out_dir, "initramfs-linux72.cpio.gz")
    dtb_src = os.path.join(out_dir, "arch/arm64/boot/dts/qcom/sm8250-oculus-hollywood-kexec.dtb")

    img = open(image_path, "rb").read()
    ksize = len(img)
    rsize = os.path.getsize(initrd_path)
    if ksize < 64:
        raise ValueError("Image < 64 bytes")
    offset = struct.unpack_from("<Q", img, 8)[0]
    memory = struct.unpack_from("<Q", img, 16)[0]
    flags = struct.unpack_from("<Q", img, 24)[0]
    magic = img[56:60]

    print(f"Image header: magic={magic!r} ksize={ksize} ({ksize/1048576:.2f} MiB) "
          f"memory={memory} ({memory/1048576:.2f} MiB) offset={offset:#x} flags={flags:#x}")

    if magic != b"ARM\x64":
        raise ValueError(f"Bad ARM64 magic {magic!r}")
    if not (64 <= ksize <= SZ_64M) or not (0 < rsize <= SZ_64M):
        raise ValueError(f"Size out of 64 MiB limit: ksize={ksize} rsize={rsize}")
    if memory < ksize or memory > SZ_64M:
        raise ValueError(f"Image memory ({memory}) invalid for 64 MiB window (ksize={ksize})")
    if offset >= SZ_2M or (offset % PAGE_SIZE) != 0:
        raise ValueError(f"Bad text_offset {offset:#x}")
    if (flags & ~0xf) != 0 or (flags & 1) != 0 or (flags & 8) == 0 or (flags & 6) not in (0, 2):
        raise ValueError(f"Unsupported Image flags {flags:#x}")

    rd = page_align(offset + memory)
    dt = page_align(rd + rsize)
    print(f"Layout offsets: offset={offset:#x} memory={memory:#x} rd={rd:#x} rsize={rsize:#x} ({rsize} B) dt={dt:#x}")

    for tag, base in (("100", 0x100000000), ("130", 0x130000000), ("906", 0x90600000)):
        out_dtb = os.path.join(out_dir, f"boot-{tag}-linux72.dtb")
        with open(dtb_src, "rb") as s, open(out_dtb, "wb") as d:
            d.write(s.read())

        initrd_start = base + rd
        initrd_end = initrd_start + rsize
        for prop, val in (("linux,initrd-start", initrd_start), ("linux,initrd-end", initrd_end)):
            hi = (val >> 32) & 0xFFFFFFFF
            lo = val & 0xFFFFFFFF
            subprocess.run(
                ["fdtput", "-t", "u", out_dtb, "/chosen", prop, str(hi), str(lo)],
                check=True,
            )

        dtb_bytes = open(out_dtb, "rb").read()
        dsize = len(dtb_bytes)
        total_bytes = align_up(dt + dsize, 0x400000)
        if dsize < 40 or dsize > SZ_2M:
            raise ValueError(f"{out_dtb}: bad dsize {dsize}")
        if total_bytes > SZ_64M:
            raise ValueError(f"{out_dtb}: total envelope {total_bytes:#x} exceeds 64 MiB ({SZ_64M:#x})")

        qkx_check_dtb(dtb_bytes, initrd_start, rsize)
        print(f"Verified {out_dtb}: dsize={dsize} initrd=[{initrd_start:#x}, {initrd_end:#x}) "
              f"total_bytes={total_bytes:#x} ({total_bytes/1048576:.2f} MiB)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
