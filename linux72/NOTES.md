# Linux 7.2 (hollywood) work log - builds #11..#13

- `patches/linux-7.2-hollywood-tracked.patch`: changes to upstream files (psci warm-reset fallback, msm a6xx, gic-v3/rpmh/spmi kexec fixes, dwc3, u_ether RX workqueue, qcom-wdt, ...). New files live in `dts/`, `drivers/`, `include/`.
- `drivers/oculus/`: SyncBoss SPI + miscfifo + fw_helpers port (`/dev/syncboss0`).
- `stage1/init`: Stage 1 initramfs init (USB ECM+ACM gadget, telnet 10.42.0.2, NBD handoff).
- `host/`: `nbd_server.py` (10.42.0.1:10809) and `kmsg_udp_recv.py` (10.42.0.1:6666 live /dev/kmsg).
- Bootargs: `panic=5 reboot=w irqchip.gicv3_pseudo_nmi=1 nmi_watchdog=panic` so lockups warm-reboot with the log kept in pstore.

## Status
- 3DoF head tracking (q2-headtrack) works; q2-desktop works.
- #12: starting SteamVR (vrcompositor) -> hard LOCKUP on CPU0/CPU3 ~2 s later (under investigation).
- #13: first Stage 2 switch hit NBD request stall -> hung_task panic -> clean pstore warm reboot (the new panic path works).
- kexec payload_base: 0x130000000 / 0x160000000 depending on free movable pageblocks.
