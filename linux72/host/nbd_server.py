#!/usr/bin/env python3
"""
NBD Server for Quest 2 Stage 2 SteamOS rootfs over USB NCM (10.42.0.1:10809).
Serves partition 1 (offset 1048576, 10 GiB) of /mnt/HDD/q2steamos/disk.img.
Supports both:
  - Oldstyle NBD handshake (BusyBox nbd-client without -N)
  - Fixed newstyle NBD handshake (BusyBox nbd-client with -N)
"""
import os
import sys
import socket
import struct
import threading

IMG_PATH = sys.argv[1] if len(sys.argv) > 1 else "/mnt/HDD/q2steamos/disk.img"
PART_OFFSET = int(sys.argv[2]) if len(sys.argv) > 2 else 2048 * 512
HOST = "10.42.0.1"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 10809

NBD_MAGIC = b"NBDMAGIC"
OLD_VERSION = 0x00420281861253
NEW_VERSION = 0x49484156454F5054  # IHAVEOPT

NBD_REQUEST_MAGIC = 0x25609513
NBD_REPLY_MAGIC = 0x67446698

NBD_CMD_READ = 0
NBD_CMD_WRITE = 1
NBD_CMD_DISC = 2
NBD_CMD_FLUSH = 3
NBD_CMD_TRIM = 4


def recvall(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("Connection closed during recv")
        buf.extend(chunk)
    return bytes(buf)


def handle_client(conn, addr, fd, export_size, mode="old"):
    print(f"[NBD] Client connected from {addr} (mode={mode}, size={export_size})", flush=True)
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 131072)
        if mode == "old":
            # Oldstyle handshake:
            # 8B "NBDMAGIC" + 8B 0x00420281861253 + 8B size + 4B flags + 124B zeros = 152 bytes
            flags = 1  # NBD_FLAG_HAS_FLAGS
            hdr = NBD_MAGIC + struct.pack(">QQI", OLD_VERSION, export_size, flags) + (b"\x00" * 124)
            conn.sendall(hdr)
        else:
            # Newstyle handshake:
            # 8B "NBDMAGIC" + 8B "IHAVEOPT" + 2B handshake_flags (NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES = 3)
            conn.sendall(NBD_MAGIC + struct.pack(">QH", NEW_VERSION, 3))
            client_flags, = struct.unpack(">I", recvall(conn, 4))
            while True:
                ihave, opt, optlen = struct.unpack(">QII", recvall(conn, 16))
                optdata = recvall(conn, optlen) if optlen else b""
                if opt == 1:  # NBD_OPT_EXPORT_NAME
                    eflags = 1  # NBD_FLAG_HAS_FLAGS
                    zeros = b"" if (client_flags & 2) else (b"\x00" * 124)
                    conn.sendall(struct.pack(">QH", export_size, eflags) + zeros)
                    break
                elif opt == 7:  # NBD_OPT_GO
                    # Send NBD_REP_INFO (3) with NBD_INFO_EXPORT (0) + NBD_REP_ACK (1)
                    info_payload = struct.pack(">HQH", 0, export_size, 1)
                    conn.sendall(struct.pack(">QIII", 0x3e889045565a9, opt, 3, len(info_payload)) + info_payload)
                    conn.sendall(struct.pack(">QIII", 0x3e889045565a9, opt, 1, 0))
                    break
                else:
                    # NBD_REP_ERR_UNSUP (2^31 + 1)
                    conn.sendall(struct.pack(">QIII", 0x3e889045565a9, opt, (1 << 31) | 1, 0))

        print(f"[NBD] Handshake complete for {addr}, entering transmission phase", flush=True)
        while True:
            req = recvall(conn, 28)
            magic, ctype, handle, offset, length = struct.unpack(">IIQQI", req)
            if magic != NBD_REQUEST_MAGIC:
                print(f"[NBD] Bad request magic: 0x{magic:08x}", flush=True)
                break
            cmd = ctype & 0xFFFF
            if cmd == NBD_CMD_READ:
                data = os.pread(fd, length, PART_OFFSET + offset)
                if len(data) < length:
                    data += b"\x00" * (length - len(data))
                reply = struct.pack(">IIQ", NBD_REPLY_MAGIC, 0, handle)
                conn.sendall(reply + data)
            elif cmd == NBD_CMD_WRITE:
                wdata = recvall(conn, length)
                os.pwrite(fd, wdata, PART_OFFSET + offset)
                reply = struct.pack(">IIQ", NBD_REPLY_MAGIC, 0, handle)
                conn.sendall(reply)
            elif cmd == NBD_CMD_DISC:
                print(f"[NBD] Client {addr} disconnected cleanly", flush=True)
                break
            elif cmd in (NBD_CMD_FLUSH, NBD_CMD_TRIM):
                os.fsync(fd)
                reply = struct.pack(">IIQ", NBD_REPLY_MAGIC, 0, handle)
                conn.sendall(reply)
            else:
                print(f"[NBD] Unsupported command {cmd}", flush=True)
                reply = struct.pack(">IIQ", NBD_REPLY_MAGIC, 22, handle)  # EINVAL
                conn.sendall(reply)
    except EOFError:
        print(f"[NBD] Connection closed by {addr}", flush=True)
    except Exception as e:
        print(f"[NBD] Error with {addr}: {e}", flush=True)
    finally:
        conn.close()


def main():
    mode = os.environ.get("NBD_MODE", "old")
    fd = os.open(IMG_PATH, os.O_RDWR)
    total_size = os.fstat(fd).st_size
    export_size = total_size - PART_OFFSET
    # Align export_size down to 4096 bytes
    export_size &= ~4095
    print(f"[NBD] Serving {IMG_PATH} (offset={PART_OFFSET}, export_size={export_size} bytes) on {HOST}:{PORT} (mode={mode})", flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.setsockopt(socket.IPPROTO_IP, 15, 1)  # IP_FREEBIND: allow binding 10.42.0.1 across USB reconnects
    srv.bind((HOST, PORT))
    srv.listen(4)
    while True:
        conn, addr = srv.accept()
        t = threading.Thread(target=handle_client, args=(conn, addr, fd, export_size, mode), daemon=True)
        t.start()


if __name__ == "__main__":
    main()
