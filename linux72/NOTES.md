# Linux 7.2 (hollywood) Work Log

## RESOLVED (2026-11-24): random core hangs after kexec = hyp/TZ-protected RAM windows

Every "freeze" in builds #12..#18 (hard core hang that ignores pseudo-NMIs, 0.6 s .. 60 s after boot, or ~9 s after
`vrcompositor` starts) was the new kernel reading a 2 MiB RAM window that Horizon's hypervisor/TrustZone had assigned
away. A plain read of such a window hangs the core; the rest of the system follows within seconds. Which windows are
protected changes on every Horizon boot, so no static reserved-memory list can work. Ruled out: GPU, cpuidle
(`cpuidle.off=1` still froze), `ADRENO_QUIRK_HAS_CACHED_COHERENT`, CPU load, preemption, static memory map mismatch,
stopping adsprpcd/cdsprpcd/trackingservice.

Found with `tools/q2memprobe.c` (static aarch64; run from Stage 1): one forked child per 2 MiB window, each pinned to
its own core, reads one word per page; a child that does not finish in 1.5 s marks the window BAD and retires that core.

**Fix:** `loader/loader.c` module parameter `claim_mb=N` claims extra MIGRATE_MOVABLE pageblocks from Horizon with
`alloc_contig_range` (blocks holding protected or pinned pages refuse and are skipped), and overwrites a 256-entry
placeholder `reg` in the DTB memory node (`tools/make_claim_dtbs.sh`) with the payload destination plus the claimed
ranges. Linux therefore only sees RAM Horizon itself can allocate (~2.4 GB at claim_mb=2560). Use `tools/kxc.sh <MB> dry`
first (stages + logs ranges, releases them), then `tools/kxc.sh <MB>` to jump.

**Verified:** full-RAM sweep of all 2483 MiB = 0 bad windows, 8/8 CPUs alive (earlier boots found 2-5 and died);
`tools/q2exp_stages.sh` (staged SteamVR escalation S0..S8 ending on the original config: vrdirect, async compositor,
hiprio, flushall, CAP_SYS_NICE at 90 Hz) passed all stages; 6+ min SteamVR soak clean.

**Second loader fix:** `native CPU shutdown failed` = `Wakeup pending. Abort CPU freeze` (`pm_wakeup_pending()` vetoing
`freeze_secondary_cpus`). loader.c now clears `events_check_enabled` / `pm_abort_suspend` after `migrate_to_reboot_cpu()`.

**Operational notes:** stop `vendor.adsprpcd*`, `vendor.cdsprpcd`, `trackingservice`, `trackingfidelityservice`, wake the
display (mWakefulness=Awake AND mScreenState=ON), `drop_caches` + `compact_memory` before kexec. Singularity root is
RAM-only and lost on every Horizon reboot; save `/sys/fs/pstore` right after re-rooting (the exploit can panic Horizon and
overwrite it). Stage 1 has no kmsg forwarder; restart `host/kmsg_udp_recv.py` and `host/nbd_server.py` after each boot
(both bind 10.42.0.1). The sections below are the earlier, pre-fix investigation.


## Repo Layout
- `patches/linux-7.2-hollywood-tracked.patch`: changes to upstream files (`psci.c` warm-reset fallback to `msm-poweroff`, `drm/msm` a6xx GMU/preempt/shutdown, `irq-gic-v3.c`, `rpmh-rsc.c`, `spmi-pmic-arb.c`, `dwc3`, `u_ether.c` RX workqueue, `qcom-wdt.c`, `phy-qcom-qmp-ufs.c`).
- `dts/sm8250-oculus-hollywood-kexec.dts`: Quest 2 kexec DTS overlay (6 GiB `/memory`, Horizon OS `no-map` reserved memory, `ramoops@9ba00000` matching Horizon OS `console-size = 0x20000`, SyncBoss `spi19`, PM8150L LCDB).
- `drivers/oculus/` & `include/`: SyncBoss SPI + `miscfifo` + `fw_helpers` port (`/dev/syncboss0`, `/dev/syncboss_stream0`).
- `stage1/init`: Stage 1 initramfs init (USB ECM+ACM gadget `qmult=16`, telnet `10.42.0.2:23`, `a650_zap` copy from `modem_b`/`modem_a` ro, NBD Stage 2 handoff).
- `host/`: `nbd_server.py` (`10.42.0.1:10809`) and `kmsg_udp_recv.py` (`10.42.0.1:6666` live `/dev/kmsg`).
- Bootargs: `rdinit=/init clk_ignore_unused pd_ignore_unused regulator_ignore_unused panic=5 reboot=w irqchip.gicv3_pseudo_nmi=1 nmi_watchdog=panic loglevel=7 printk.devkmsg=on console=tty0 fbcon=font:TER16x32 fbcon=rotate:3 drm_client_lib.active=fbdev`


## UPDATE (2026-11-24 evening): RAM size, fan, buttons, controllers

- **RAM:** `kxc.sh <MB> dry` first. With Horizon's apps killed and caches dropped, 4 MiB-granular claims (`claim_step_kb=4096`,
  `claim_relax=1`) reached ~3.7 GB in dry runs (167 ranges); the real jump gave Linux ~3.0 GB. 2 MiB steps are *worse*: once
  the first half of a pageblock is claimed, `alloc_contig_range` refuses the second. Needs the kernel patch
  `INIT_MEMBLOCK_REGIONS 2048` (mm/memblock.c): arm64 only enables memblock resizing in `paging_init()`, after the DTB memory
  node is parsed, so >128 memory ranges were silently dropped. That patch grows the Image header size, so `rd` moves
  (0x33a0000 -> 0x33b0000): `make_claim_dtbs.sh` reads it from the Image. Further RAM is limited by ~450 pageblocks holding
  pinned/unmovable pages in Horizon's kernel; stopping Android's framework gained <100 MB.
- **Fan:** PM8150L PWM fan, LPG channel index 4 (`0xbd00`, Quest OS `pm8150l_pwm 1`), pad gpio10 `func1`, tach gpio64. The PM8150L
  GPIO controller is on **SID 4** (LPG on SID 5): putting it on SID 5 gives `read 0x4 failed` and the `pwm-fan` stays deferred.
  Stopgap that works with no DT change: `echo 4 > /sys/class/pwm/pwmchip0/export; echo 40000 > pwm4/period; echo 40000 > pwm4/duty_cycle; echo 1 > pwm4/enable`
  (the pad mux Horizon left in the PMIC survives kexec). The DTS now also reserves gpio pads 2/9/12 (Quest OS `gpios-disallowed`);
  the SID 4 version is built but not yet booted.
- **Thermals:** `performance` governor + `cpuidle.off=1` (diagnostic settings) reached 90 C and a UFS/SError panic. Use `schedutil`
  and normal idle states, keep the fan at 100 %.
- **Headset buttons:** the NBD Stage 2 setup masks `q2-buttons.service` and sets `/var/lib/q2/nobuttons`. To enable: remove the flag,
  install the unit text from `stage2_prep.sh`, add `HandlePowerKey=ignore` in `logind.conf.d`, and extend `q2buttons`' device match
  with `pm8941_pwrkey` / `pm8941_resin` (this kernel names them so; volume-down arrives as `pm8941_resin`). Volume has no effect
  until audio works.
- **Controllers (open):** the tracker sends beacon command `0x85` every 20 s and gets only IMU packets (types 01/50), never `0x8f`
  controller packets. Linux's SyncBoss driver pin-resets the MCU when first opened, and the services that Horizon's pairing may
  depend on (`trackingservice`) were stopped before the jump (no longer needed since the claim approach). Next test: jump without
  stopping Horizon's tracking services and compare.
- **Rootfs gotchas:** unit fixes written in Stage 2 land in a RAM overlay of `/etc`; fix on-disk files from Stage 1 while the NBD root
  is mounted. `q2-headtrack.service` is `->/dev/null` on disk from an earlier mask and must be restored from the text in
  `stage2_prep.sh`.

---

## Verified Working on Mainline Linux 7.2
- **Warm-reboot `pstore` retention**: `reboot=w` + `psci.c` returning `NOTIFY_DONE` on `REBOOT_WARM` (`psci: warm reset refused: -2`) falls through to `qcom,pshold` (`msm-poweroff`) and preserves `/sys/fs/pstore/console-ramoops-0` across panics.
- **Hardware GICv3 Pseudo-NMIs & PMU NMI Watchdog**: `CONFIG_ARM64_PSEUDO_NMI=y` + `irqchip.gicv3_pseudo_nmi=1` initializes cleanly (`GICv3: Pseudo-NMIs enabled using forced ICC_PMR_EL1 synchronisation`, `hw perfevents: armv8_pmuv3 ... using NMIs`, `NMI watchdog: Enabled`).
- **Stage 1 + Stage 2 SteamOS over NBD**: `qmult=16` is stable (avoid `qmult=64`, which caused NBD request stalls on `#13`). Wait for `/sys/block/nbd0/size != 0` before `mount -t btrfs /dev/nbd0 /mnt/newroot`.
- **3DoF Head Tracking (`q2-headtrack`)**: SyncBoss SPI runs reliably at `~1,011–1,012 samples/s`, 0 SPI errors, `/dev/shm/q2pose` updating at `~342 Hz`.
- **Standalone GPU (`q2gputest`)**: All 7 hardware tests pass (Turnip Adreno 650, `VK_EXT_physical_device_drm`, GBM, EGL Mesa, Freedreno `FD650` GL draw/readback, `dma-buf` `EXPORT_SYNC_FILE`/`IMPORT_SYNC_FILE`, KMS `PRIME` import + `ADDFB2`).
- **Desktop (`q2-desktop`)**: `Xvfb` + `q2kmsd` + `q2blit` + `openbox` runs stably for 8+ minutes (uses `llvmpipe` software GL).
- **SteamVR Rendering**: `vrserver` + `vrcompositor` actually rendered the default SteamVR mountains & aurora scene on the Quest 2 panel via Turnip Adreno 650 + KMS (`#16`), before hitting the freeze below.

---

## Build-by-Build Forensic Log (#12..#16)

### #12 (Baseline: `PSEUDO_NMI=n`, `DEBUG_SPINLOCK=n`, `qmult=16`)
- Stage 2, `q2-headtrack`, `q2gputest`, and `q2-desktop` ran cleanly for 474 s (~8 min).
- ~2 s after `vrcompositor` started: `watchdog: CPU7: Watchdog detected hard LOCKUP on cpu 0`, then `CPU2: Watchdog detected hard LOCKUP on cpu 3`, `rcu_preempt` stall on CPU 3 (`idle=2e84/1/... softirq=23794/23795`), `After 10 seconds, these CPUS still haven't responded to the NMI: 0`.

### #13 (`PSEUDO_NMI=y`, `DEBUG_SPINLOCK=y`, `qmult=64`)
- Verified `pstore` warm-reboot retention (`79 KB` `console-ramoops-0` preserved) and verified `GICv3: Pseudo-NMIs` + `NMI watchdog` work on Quest 2.
- Stalled on `nbd0` (`Possible stuck request ... Runtime 60 seconds` -> `hung_task` panic on `btrfs-transacti`) due to `qmult=64` (128 requests per endpoint against DWC3's 256-TRB ring).
- Fixed `workqueue: u_ether_rx is using neither WQ_PERCPU or WQ_UNBOUND` warning by adding `WQ_PERCPU`.

### #14 (`PSEUDO_NMI=n`, `DEBUG_SPINLOCK=y`, `qmult=16`, `WQ_PERCPU`)
- Mounted `/dev/nbd0` cleanly (`mount_rc=0`), switched root at `29.70s`.
- At `~30.3s` (0.6 s after `switch_root`, before `systemd` printed its first line, with **no GPU/SteamVR/SyncBoss running**): **CPU 7** (`Cortex-A77 r1p0`) hard-locked (`watchdog: CPU6: Watchdog detected hard LOCKUP on cpu 7` at `44.01s`, `qcom_wdt` bark `watchdog pretimeout event` at `44.34s`, `SMP: failed to stop secondary CPUs 6-7`).
- **Ruled out**: contended kernel `spin_lock` (`CONFIG_DEBUG_SPINLOCK=y` printed zero spinlock lockup warnings over 14 s) and NBD stall (`Send-Q=0`).

### #15 (`PSEUDO_NMI=y`, `CSD_LOCK_WAIT_DEBUG=y`, `DEBUG_SPINLOCK=y`, `qmult=16`, `cpuidle state1` disabled live)
- Booted cleanly into Stage 2; disabled `cpuidle state1` (`silver/gold-rail-power-collapse`) on all 8 CPUs via sysfs before starting `q2-desktop` + `q2-vr`.
- `vrserver` + `vrcompositor` + `q2-headtrack` ran ~14 s before **CPU 5** (`0x411fd0d0` = Cortex-A77 r1p0) froze while holding `sk_lock` on the NBD TCP socket (`sync:2813` blocked in `__lock_sock`, causing NBD `Send-Q=209960` and `hung_task` panic at `302.8s`).
- **Critical finding from `ramoops_15.log`**:
  - Hardware GICv3 Pseudo-NMIs worked (`CPU 0` answered CPU 6's NMI instantly), **yet CPU 5 and CPU 3 ignored both their own PMU NMIs and GICv3 Pseudo-NMI SGIs** (`After 10 seconds, these CPUS still haven't responded to the NMI: 5`; `SMP: retry stop with NMI for CPUs 3,5` -> `failed to stop secondary CPUs 3,5`).
  - **Ruled out**: `cpuidle state1` (`rail-power-collapse`) and normal `DAIF.I=1` / `spin_lock` hangs (which Pseudo-NMI preempts).

### #16 (Enabled Cortex-A77/A55 CPU errata + Fixed Horizon OS Reserved Memory)
- **Fix 1 — Missing ARM64 Silicon Errata**: Base config had disabled upstream `default y` errata, including:
  - `CONFIG_ARM64_ERRATUM_1508412=y` (**Cortex-A77 r0p0–r1p0 silicon deadlock** on Device/NC load + `STXR`/PAR; CPUs 4–7 are `0x411fd0d0` = Cortex-A77 r1p0)
  - `CONFIG_ARM64_ERRATUM_1530923=y`, `1024718=y`, `2441007=y` (CPUs 0–3 are `0x51df805e` = Kryo 4xx/5xx Silver / Cortex-A55)
  - `CONFIG_ARM64_ERRATUM_1542419=y`, `1286807=y`, `1463225=y`, `843419=y`
  - Verified active in `dmesg`: `ARM erratum 1508412 (kernel portion)` and `ARM errata 1165522, 1319367, or 1530923`.
- **Fix 2 — Missing Horizon OS Reserved Memory (`no-map`)**:
  - Live Horizon OS `/proc/iomem` + `dmesg` showed `0xec800000..0xffc1ffff` (`308 MiB`, contiguous) is reserved for `secure_display_region` (164M), `user_contig_region` (16M), `sdsp_region` (8M), `qseecom_ta_region` (16M), **`cnss_wlan_region` (20M, XPU-locked to `VMID_WLAN`)**, `sp_region` (16M), `cdsp_region` (4M), `adsp_region` (12M), `qseecom_region` (20M), `linux,cma` (32M), and `mailbox_region` (128K). Previously Linux 7.2's SWIOTLB (`0xf4400000..0xf8400000`) overlapped `sdsp_region`/`secure_display_region`, and `cnss_wlan_region` was unreserved.
  - Added `horizon_tz_cma_region@ec800000` (`reg = <0x0 0xec800000 0x0 0x13420000>; no-map;`) in `sm8250-oculus-hollywood-kexec.dts`. Verified SWIOTLB relocated cleanly to `0xe8800000..0xec800000`.
- **Fix 3 — Stage 1 `init` cleanup**:
  - Updated lines 59–60 of `stage1/init` (`while [ -x /bin/dmesg ]` and `while [ -x /bin/sh ]`) so background loops exit cleanly after `switch_root` instead of spamming `dmesg: not found` on the console. Rebuilt `initramfs-linux72.cpio.gz` (`1,851,127` bytes) and repacked boot DTBs.
- **Test Results on #16 (both before and after `horizon_tz_cma_region@ec800000`)**:
  - Boot, `q2-headtrack` (`1,012 samples/s`), and `q2-desktop` are 100% stable (8+ min, zero boot/idle lockups).
  - **Still reproduces on `vrcompositor`**: ~6–9 s after `vrcompositor` starts rendering the SteamVR mountains & aurora scene, the display freezes and `usb0` stops responding (`NBD` queues `0/0` on the second run, `qcom_wdt` still petted by a live CPU so it does not auto-reboot).

---

## Current Root-Cause Narrowing & Next Steps
Everything ruled out for the `vrcompositor` freeze:
- NOT NBD stalls (`Send-Q=0, Recv-Q=0` on `#16` run 2)
- NOT `cpuidle state1` (reproduced with `state1` disabled on `#15`)
- NOT contended `spin_lock` (`DEBUG_SPINLOCK=y` silent, and Pseudo-NMI preempts `spin_lock`)
- NOT missing Cortex-A77/A55 errata (`1508412`/`1530923` active on `#16`)
- NOT unreserved Horizon OS memory (`0xec800000..0xffc1ffff` `no-map` active on `#16` run 2)

Only **Adreno 650 GPU / `vrcompositor`** (`3d00000.gpu`, `3d6a000.gmu`, `3da0000.iommu` `adreno_smmu`, `vrdirect=1`, `TU_DEBUG=nobin,hiprio,flushall`, `compositor async`, `CAP_SYS_NICE`) is exercised by `vrcompositor` and not by `q2-desktop` (`llvmpipe`).

### Next Single-Boot Isolation Plan
1. Push updated `initramfs-linux72.cpio.gz` + repacked `boot-*-linux72.dtb` (`horizon_tz_cma_region@ec800000`, `rd=0x33a0000`) and `sync`.
2. In Stage 1 before `switch_root`, install persistent Stage 2 `q2-telnet.service` on port **`2323`** (no collision with Stage 1 port `23`) and `sync`.
3. Run a **sustained standalone Adreno 650 Vulkan stress test** (`q2gputest` loop, 15+ iterations) *before* starting `vrcompositor` to separate a kernel `a6xx_gpu`/`gmu`/`adreno_smmu` bug from a `vrcompositor`-specific flag.
4. If standalone GPU passes, test `q2-vr` with `echo 0 > /var/lib/q2/vrdirect` and/or without `TU_DEBUG=hiprio` / `CAP_SYS_NICE` / `compositor async` to pinpoint the exact trigger.
5. Once stable, set up Linux→Linux `/bin/q2kexec` (eliminate Horizon OS reboots for kernel iteration) and proceed to 6DoF OV7251 (`cci0`/`cci1`/`camss`) bring-up.

---

## Operational Notes
- `quest_kexec.ko` parameters: `image=`, `dtb=`, `initrd=`, `payload_base=` (`0x100000000`/`0x130000000`/`0x140000000`/`0x160000000`/`0x180000000`), `stage2_verified=1 shutdown_dsps=1 watchdog_recovery=1 preserve_watchdog=1 execute=1`.
- Always recompute `rd = PAGE_ALIGN(offset + memory)` from the `Image` header when repacking `boot-*-linux72.dtb` (`initrd-start = base + rd`, `initrd-end = initrd-start + initramfs_size`).
- Always `sync` after pushing files to `/data/local/tmp/`, and verify `mWakefulness=Awake` + `mScreenState=ON` (`am broadcast -a com.oculus.vrpowermanager.prox_close`) before kexec.
