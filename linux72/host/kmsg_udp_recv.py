#!/usr/bin/env python3
"""Host-side live UDP kmsg collector bound only to 10.42.0.1:6666."""

import datetime
import socket
import sys

BIND_IP = "10.42.0.1"
BIND_PORT = 6666
LOG_PATH = "/mnt/HDD/quest2-linux72/stage2/kmsg_live.log"
IP_FREEBIND = 15


def main() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.IPPROTO_IP, IP_FREEBIND, 1)
    sock.bind((BIND_IP, BIND_PORT))

    with open(LOG_PATH, "a", encoding="utf-8", errors="replace") as logf:
        logf.write(
            f"[{datetime.datetime.now().isoformat(timespec='milliseconds')}] "
            f"kmsg_udp_recv listening on {BIND_IP}:{BIND_PORT}\n"
        )
        logf.flush()
        while True:
            data, addr = sock.recvfrom(65535)
            ts = datetime.datetime.now().isoformat(timespec="milliseconds")
            text = data.decode("utf-8", errors="replace").rstrip("\r\n")
            for line in text.splitlines() or [""]:
                logf.write(f"[{ts}] {addr[0]}:{addr[1]} | {line}\n")
            logf.flush()


if __name__ == "__main__":
    sys.exit(main())
