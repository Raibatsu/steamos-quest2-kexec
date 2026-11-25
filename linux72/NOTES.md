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


## MERGED (2026-11-25): upstream's linux72 layer + our kexec changes

Upstream (URFAVF3MBOY/quest2-steamos, `kernel/linux72`) now carries the Android compat layer (binder, ashmem, ION, downstream `fastrpc` ABI),
Qualcomm's Spectra camera drivers from Meta's kernel (`techpack/camera`, ~149k lines) with CCI/CSIPHY/IFE/sensor DT nodes, and a new SyncBoss
driver. They use these to run **Meta's own tracker** (from the headset's stock partitions) for head and controller *position*; their direct
four-camera path (`q2camd`/`q2blobs`/`q2ctltrack`) is built but not started. As of 2026-11-24 no commit shows cameras streaming on 7.2.

Recipe (all scripted): clean worktree at the base commit -> `tools/apply_upstream_layer.sh <tree> <upstream checkout>` (mirrors their
`build-linux72.yml`: DTS, patches/*.patch, panel, `drivers/oculus`, `drivers/android` -> `drivers/misc/q2android`, `camera` -> `techpack/camera`)
-> `git apply patches/kexec-over-upstream.patch` (our psci/gic/rpmh/spmi/dwc3/u_ether/wdt/aoss/a6xx-gmu/memblock changes; hunks upstream already
has are dropped) -> copy `dts/sm8250-oculus-hollywood-kexec.dts`, `hollywood-kexec.config`, `drivers/qcom-pm8150l-lcdb.c`.
Config: `gabe_defconfig` + upstream `hollywood.config` + our `hollywood-kexec.config` (which now records the CPU errata, pseudo-NMI and
hard-lockup options that used to be set by hand). The merged kernel boots, and head/controller/fan/button services all pass a regression check.

Things that needed care:
- **Do not overlay our SyncBoss node**: upstream's new driver uses `oculus,syncboss-*-gpios` and its own pin states; our overlay no longer defines one.
  Upstream independently has `regulator-always-on` on L7C/L8C (our reopen-wedge fix).
- **Panel**: upstream's panel driver only toggles enable/reset and assumes the bootloader left rails on and the backlight lit; after a kexec the
  panel is dark (`panel off at probe`) and it has no backlight device. We keep **our** `panel-oculus-hollywood-jdi.c` (init sequence, backlight,
  vddio/lcdb/blu-mcu supplies) plus the overlay's `&apps_rsc` L14A/L11C nodes, the LCDB node and the `&mdss_dsi0 panel@0` supplies.
- UFS PHY: upstream patch 0001 replaces ours; applying both gives a duplicate `hollywood_ufsphy_cfg`.
- `ife_dsp_clk` / `DSI PLL lock failed` messages are benign. Camera probe shows ISP + both CCI controllers; sensor probing is not exercised yet.
- Iterate with Linux->Linux kexec: `tools/make_q2kexec_dtb.py` + `q2kexec -B`.

## FIXED (2026-11-25): controllers and the SyncBoss reopen wedge

1. **Beacon payload.** The MCU's radio only enters beacon mode when `0x85` carries a payload byte: `[0x85, seq, 1, 0x01]`
   (frame = type, seq, len, data). Empty `0x85` is acked with status 0 but `GET_DATA 0xd2` stays `0`; with payload `01` it reads `2` and
   0x8f controller packets flow at ~500/s per controller. `/usr/bin/q2headtrack` line `bytes([0x85, 2, 0])` -> `bytes([0x85, 2, 1, 1])`.
   (Backup: `q2headtrack.orig`.) The pairing list (`GET_DATA 0x7c`, 193 bytes, count 2 = left+right) was already on the MCU: no pairing needed.
2. **`SET_DATA 0xd5` does not exist on the Quest 2 MCU** (replies -22, MCU log: `Unknown item set requested (item 213)`): Quest 1 only; do not send it.
3. **Command replies come on `/dev/syncboss_control0`** (not the stream, not `/dev/syncboss0`), and MCU text logs arrive as type `0x92`
   (`tools/sbprobe.py`, `tools/sbprobe2.py`).
4. **Reopen wedge:** after the first close of `/dev/syncboss0` the MCU acked everything but never streamed IMU again until a Horizon boot.
   Cause: the vendor driver's close drops the IMU (L7C 3.0 V) and mag (L8C 1.8 V) rails; in Quest OS other consumers keep them up, here
   nothing does, so the next open pin-resets the MCU onto a cold IMU. Fix in the DTS: `regulator-always-on` + `regulator-boot-on` on both.
   Verified: tracker restarted repeatedly and SteamVR stop/start no longer wedge it. (Driver unbind/rebind is still broken: the nsync misc
   device is not removed, `-EEXIST` on rebind.)
5. Headset battery/prox: unchanged. Controller firmware/pairing are untouched; nothing here pairs, unpairs or updates anything.

## UPDATE (2026-11-24 night): Linux->Linux kexec works; controller radio diagnosis

- **Linux->Linux kexec (no Horizon, no Singularity) works.** The old `q2kexec` unbound the USB gadget and *then* called `sync()`; with the
  Stage 2 root on NBD over that same USB link, btrfs commit blocked forever (`nbd0: Possible stuck request`) until hung-task reset.
  Fixed: sync first, then unbind. New option `-B <hex base>` places the kernel inside RAM claimed by the loader. Flow: fetch the running
  kernel's `/sys/firmware/fdt`, `tools/make_q2kexec_dtb.py live.dtb kexec.dtb Image initrd out.dtb` (prints the base), then
  `q2kexec -l -B <base> -D out.dtb Image initrd` (load-only check) and `q2kexec -B <base> -D out.dtb Image initrd`. New kernel is back in ~4 s.
- **DT fan verified:** `pwmfan` hwmon, pwm1=255, tach ~9500 RPM, with the PM8150L GPIO controller on SID 4 and pads 2/9/12 reserved.
- **Controllers (still open).** Reference (nicolas-riera/quest1-steamos controllers-native.md, Quest 1 static analysis): the pairing list is
  on the MCU (get_data 0x7c), beacon mode state is get_data 0xd2 (2 = beacon), and the vendor `input_start` first does set_data 0xd5 = 0,
  then beacon cmd 0x85; data only decodes in beacon mode, and controller packets are dropped silently if the controller firmware does not match
  the library's expectation (LCON app 1.17.2 / SPL 1.13.0 for Quest 1). Frames on /dev/syncboss0: {type, seq, len, data}; GET_DATA=2, SET_DATA=3.
  `tools/sbprobe.py` reads 0xd2/0x7c, sets 0xd5=0, starts beacon and dumps non-IMU records. Never sends pair/unpair (0xd1).
  Tested so far: leaving Horizon's trackingservice running before the jump does NOT make controllers appear.
- **SyncBoss wedge:** after stopping SteamVR + the tracker, every later open gets an MCU that acknowledges transactions (spi/ready IRQs rise,
  stats show only empty transactions) but streams no IMU records, even on a fresh kernel (kexec) and with `control/reset`. Driver unbind/rebind is
  broken (`failed to register nsync misc device, error -17`: nsync misc device not removed on unbind). Only a Horizon boot has recovered the MCU
  so far. Run `sbprobe.py` FIRST after a Horizon->Linux jump, before SteamVR or the tracker have touched the MCU.

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

## Eye alignment and hardware check (2026-11-25)

Eye tuning on the merged kernel (SteamVR, slider position 2 = stock):
- `/var/lib/q2/eyeshift` (px, + = pictures toward the nose) **does** work in SteamVR's zero-copy path, but only read at VR start. Findings by eye:
  +275 = two separate screens, +130/+90 worse than +40, +20/0/-20 look the same. **+40 is the current best**; try 40-90 to refine.
- `/var/lib/q2/ipdmm` (virtual eye separation, mm) is live in SteamVR but the driver ignores anything below **50 mm** (min applied was 50).
- `Q2VR_LENSADJ` (read by driver_q2hmd) is in **tenths of a mm** (30 = +3 mm toward the nose). It was never set by the launcher; `rootfs-patches/eyes/patch_lens3.py`
  adds `export Q2VR_LENSADJ=$(awk ...)` from `/var/lib/q2/lensadj`. Gotcha: q2vrtest writes /tmp/q2vr-run.sh from an *unquoted* heredoc, so a
  `[ -n "$VAR" ] || VAR=0` fallback expands at write time and always zeroes the value. Lens +3 mm at slider 3 fixed black outer edges there;
  at slider 2 the stock lens has no black edges.
- Reproducible instant-feedback tool: `tools/q2calib.c` (static crosshair + 0..18 ruler per eye, shift read live from /var/lib/q2/eyeshift). Its
  ruler gave the measurement (left eye hides ~200 px on its outer side, right ~140 px, so ~85 px / ~2.9 mm toward the nose, ~29 px per mm).
  After running it the display must be re-established with a kexec: restarting q2-kmsd while it owns the panel breaks SteamVR output
  ("KMS: the display was taken over but q2kmsd is gone - reboot to get the picture back").
- SteamVR supervisor: after `q2vr stop` the next `q2vr start` is often dropped until `systemctl restart q2-vr`. Never `kill -9` the VR processes.
- `rootfs-patches/eyes/*.py` are the on-device patch scripts for /usr/bin/q2buttons (volume keys) and /usr/bin/q2vrtest, in order of use. The rootfs is RAM-only,
  so these must be re-applied after a Horizon reboot (the rootfs image is on the host and keeps them while the NBD disk is reused).

`tools/hwcheck.sh` is the read-only hardware check. Results on the merged kernel:
| Block | Result | Cause |
|---|---|---|
| Audio | **still missing** | the speakers and mics sit behind an AKM AK4331 (headphone DAC) and a C-Media CM710x DSP codec (I2C 0x2c, needs `CM710X.bin`); neither has a mainline driver. Upstream's own notes say there is no 7.2 driver either |
| Wi-Fi | **works** | QCA6390 on PCIe0 via ath11k (`qca6390 hw2.0`, fw 0x10121492); scans find real networks |
| Bluetooth | **works** | QCA6390 on UART6 via hci_qca; controller `02:51:4B:58:00:B7` (set in the DT), scan finds devices |
| Battery | **works (gauge only)** | TI BQ27Z561 on i2c4 @0x55: charge, voltage, temperature. The SMB5 charger has no mainline driver, so no charger status |
| Sensors | only the PMIC ADC (iio) | IMU, magnetometer and proximity come through SyncBoss (`oculus,syncboss-has-prox`), which works |

### Wi-Fi, Bluetooth and battery bring-up (2026-11-25)
- Device tree (`dts/sm8250-oculus-hollywood-kexec.dts`): `qca6390-pmu` power sequencer (WLAN_EN gpio20, BT_EN gpio21; supplies S6A 0.95 V, S4A 1.8 V,
  S5A 1.9 V, S8C 1.35 V; pins and rails read from Horizon's tree), `&pcie0` + `&pcie0_phy` + `wifi@0` (`pci17cb,1101`), `&uart6` + `bluetooth` (`qcom,qca6390-bt`),
  `serial1 = &uart6` (without the alias qcom_geni_serial refuses the port: "Invalid line -19"), `local-bd-address` (without it BlueZ treats the chip as
  unconfigured and shows no controller), and `&i2c4` with the gauge. `&gpi_dma0` and `&qupv3_id_0` must be enabled for i2c4.
- Config: `PCI_PWRCTRL_PWRSEQ=y` (was `m`), `ATH11K=y`, `ATH11K_PCI=y`, `BATTERY_BQ27XXX(_I2C)=y`.
- Firmware is not in the repo. Wi-Fi: copy `amss.bin`, `m3.bin`, `board-2.bin` from linux-firmware `ath11k/QCA6390/hw2.0/` to the rootfs. Bluetooth: `qca/htbtfw20.tlv`
  and `qca/htnv20.bin` were already on the rootfs.
- Stage 1's initramfs has no firmware, so both drivers fail there. `rootfs-patches/radios/q2radios` + `q2-radios.service` re-probe them once Stage 2 is up
  (unbind/bind `0000:01:00.0` from ath11k_pci, and `serial0-0` from hci_uart_qca). Gotcha: do not order that unit Before=bluetooth.service and then
  `systemctl restart bluetooth` from inside it; use `--no-block`, or it deadlocks.
- The clk warnings at ~2.2 s (clk_core_disable/unprepare, deferred_probe_work_func) are from the remoteproc/cdsp probe, not from these changes.

## Camera bring-up on 7.2 (2026-11-25)

Tools are upstream's `image/src/q2cam/*` (URFAVF3MBOY/quest2-steamos), built from source here with
`aarch64-linux-gnu-gcc -O2 -ffreestanding -nostdlib -static -fno-stack-protector -fno-builtin -I inc -w` (freestanding, raw syscalls only; reviewed before running).

| Stage | Result |
|---|---|
| ISP test pattern (`q2cam_tpg`: CSID TPG -> IFE fastpath RDI0, no sensor) | **works**: a 640x480 frame in 63 ms, clean teardown (one `CSID halt failed -110` at stop) |
| Sensor subdevs | 4x `cam-sensor-driver`, 6x `cam-csiphy-driver`, `cam-isp`, `cam-isp-fastpath`, `cam-cpas`, 13 subdevs, `/dev/media0,1`, `/dev/video0,1` |
| Sensor power (`q2camlive`) | slots 0,1,2 **power up** (L10C 3.0 V vana, S4A 1.8 V vio, MCLK0-2 at 19.2 MHz, verified in clk_summary/regulator_summary); **slot 3 fails EINVAL**: gpio95 (its MCLK pin) is held as a plain GPIO ("f100000.pinctrl:629") instead of cam_mclk |
| Frames | **works**: 3 sensors x ~59.8 fps, ~880 frames each in 15 s, hardware timestamps 16.7 ms apart, real images (640x481 incl. one metadata row) |

**RESOLVED (2026-11-25): the camera command sequence.** The MCU was rejecting upstream's strings because they carry no camera mask and skip `init`. Meta's own
`libsyncboss.so` (`odm_a/lib64`, read from the stock partitions) builds every camera start/stop/init call through one helper: command byte, then a **4-byte
little-endian camera bitmask** as payload (timeout 125). `camera_v2_start_streaming` = `0x2c`, `stop_streaming` = `0x2d`, **`init` = `0x2e`**, `deinit` = `0x2f`;
`init` then loads gamma tables and sends them with `0x25`, and `set_frame_config_v2` uses `0xb1` (not needed for streaming). The working sequence is
`2d 00 04 <mask>`, `2e 00 04 <mask>`, `2c 00 04 <mask>` ... capture ... `2d 00 04 <mask>` with mask `07000000` (slots 0-2; use `0f000000` once slot 3 powers up).
The old `0x28` probe and `0xdd` set_metadata_mode steps are not needed. `tools/q2camlive-mask-init.patch` applies that change to upstream's `q2camlive_main.c`;
`rootfs-patches/camera/patch_wl.py` adds `0x2e`/`0x2f` to q2headtrack's camera-command whitelist (it already forwarded 0x2a/0x2c/0x2d/0x28 and 0x03..0xdd).
Verified through the live tracker: 2643 images in 15 s, controllers kept streaming. The earlier notes below (command rejections) describe the old, incomplete sequence.

**Slot 3 fixed (2026-11-25):** `sm8250-oculus-hollywood.dts` had the clock pin-mux states of sensors 1 and 3 swapped (sensor1 pinctrl gpio97 + gpio request 95; sensor3 pinctrl gpio95 + request 97), so
sensor 3's gpio97 request collided with sensor 1's mux (`pin-97 EINVAL`). `patches/camera-sensor-pinctrl.patch` swaps them (apply_upstream_layer.sh applies it). All four sensors now stream ~59.8 fps (3512 images in 15 s, mask `0f000000`).

Still open: turning frames into tracking (`q2blobs`/`q2ctltrack`, or Meta's tracker).

### Meta's stock userspace, extracted for building native replacements (2026-11-25)
`tools/q2stockmount.py mount|umount` mounts the stock partitions of `super` (vendor_a, odm_a, product_a, system_ext_a, system_a, all ext4) **read-only** (dm --readonly, ro,noload,
a 1 MiB margin after the last extent because vendor_a's ext4 block count is 15 blocks larger than its LP extents, which makes ext4 refuse the mount otherwise). It was used to copy
vendor_a and odm_a whole plus the oculus/sensor/ossdk/tracking libraries of system_ext_a to the host (458 MB, 1122 files, kept OUT of git under `quest2-linux72/meta-extract/files/`):
- SyncBoss/camera: `libsyncboss.so`, `libsyncbossmanager.so`, `syncboss.bin` (MCU firmware) + `syncboss-unwind.bin`, `syncboss_consumers_ctl`, the four `com.qti.sensormodule.liteon_ov7251_*_syncboss_cam_N.bin`,
  `chicamera/com.qti.tuned.default.bin`, `firmware/CAMERA_ICP.elf`, `libfastpathcamerahal.so`, `libcamxcsl.so`, `libcamerahal.so`, `libxrcameratal.so`, `cameramuxmode/configv2.json`
- Sensors/OS SDK: `vendor.oculus.hardware.sensors@1.0-service`, `vrsensorsctl`, `libvrsensors-*`, `libsensor*`, `libossdk.oculus.so`, `allowedOSSDKEvents.json`
- Tracking: `trackingservice` + `trackingservice.cfg`, `libtrackingengines.so` (44 MB), `libhzos_tracking*`, hand/face/body/passive-tracking models under `odm_a/etc`
- DSP/audio: `vendor_a/dsp`, `lib/rfsa/adsp` skeleton libs, `cdsprpcd`/`adsprpcd`, `libacdb*`, `etc/acdbdata/*`, `audio_policy*.xml`
Nothing there is committed (firmware and vendor binaries, and the persist partition was never copied). Absolute-path symlinks were skipped on extraction (tarfile data filter).
Why no frames: the SyncBoss MCU starts the sensors, and it **rejects the command strings q2camlive sends** (found with `tools/sbcamprobe.py` / `tools/sbproxy.py`, which print the MCU's
own log records, type 0x92 and 0xd8, while the tracker is stopped):
- `2c0000` / `2d0400` (start / stop, no payload) -> `general_cam_mgr.c: Invalid camera ids provided: 0x0, num_cams: 4`
- With a one-byte camera mask (`2c0001ff`, `2d0401ff`): the MCU walks `ov7251[0x1] [0x2] [0x4] [0x8]` and says `not yet initialized` (start) / `not configured` (stop)
- `030004dd000101` (register 0xdd, 3 data bytes) -> `{HIFH}: set_metadata_mode: bad data_size=3`; sizes 1 and 2 are also rejected (4 or more not tried)
- `280000` / `280001ff` (camera probe) -> a type 0x46 stream packet with `00`, no log line; the cameras stay "not initialized"
- Same replies with the sensors powered by the AP, so it is not a power-sequencing problem.
Frame format is {type, seq, len, data}: `2c 00 01 ff` = type 0x2c, seq 0, len 1, data ff.
Hypothesis: this MCU firmware wants explicit payloads (camera mask, init/config data) that upstream's strings, written against Quest OS 4.19, omit. Next: read Meta's `libsyncboss`/sensors
service in the stock partitions (read-only, `q2stockcam.py` parses the super partition) to get the real camera init command, or run Meta's own tracker as upstream does.
Gotchas: `q2camlive` needs `/run/q2headtrack.pid`; `sbproxy.py` removes it on exit, so restart `q2-headtrack` afterwards. The camera commands briefly stall the MCU link
("bad magic from MCU" x10, IMU pauses ~5 s, controllers drop) and recover on their own.

### Camera sessions: why the second one failed, and the fix (2026-11-25)
Symptom: the first camera session after a q2-headtrack restart streamed ~59 fps x 4, every later one got 0 frames (also after `q2camlive daemon` was killed). The MCU log (read passively with
`tools/mculog.py` next to the live tracker) shows why: when the process dies the kernel powers the sensors down, the MCU can no longer stop them (`Failed writing I2C, reg=0x0100 ... Failed to stop streaming`),
and the next `init` hits `ov7251.c: OV7251 already initialized` / `Camera ov7251[0x1] is not yet initialized`. A tracker restart resets the SPI link and the MCU's state, which is why that "fixed" it,
and why a fresh stand-in process (sbproxy) always worked.
Fix (all in place): every session is `2d`, `2f` (deinit), `2e`, `2c` and ends with `2d`, `2f`, each with mask `0f000000` (`tools/q2camlive-mask-init.patch`); the tracker's forwarder waits
after the slow commands (`rootfs-patches/camera/patch_delay.py`: init 1.5 s, stop/deinit 0.4 s, start 0.3 s, others 0.05 s, because the MCU reads every sensor's chip ID over I2C during init);
`rootfs-patches/camera/q2-camd.service` stops the sensors with `ExecStop` (before the daemon is killed). Verified: daemon, abrupt kill, daemon, clean stop, daemon, and systemd start/stop/start all give 57-60 fps on 4 cameras.
In `daemon` mode the "fps per camera" log line is always 0 (the counters live in the forked readers); use `tools/shmcheck.py` (reads the published-frame counters of /dev/shm/q2cam) instead.

### Cameras start by themselves at boot (2026-11-25)
`q2-camd` (q2camlive daemon, all four sensors -> /dev/shm/q2cam, 60 fps) and `q2-ctltrack` (/dev/shm/q2cam -> /dev/shm/q2ctlpos) are enabled; `q2-radios` (Wi-Fi/BT re-probe) too.
Opt out with `touch /var/lib/q2/nocameras`. Verified over two reboots (kexec): after Stage 2 comes up all four cameras publish 60 fps with no manual step.
**Gotcha that cost a reboot:** the rootfs mounts `/etc` as an overlay (`lowerdir=/etc, upperdir=/var/lib/overlays/etc/upper`) AFTER systemd has read its units. Unit files and `*.wants` links written to the
running `/etc` land in the upper layer: they persist on the btrfs root and `systemctl is-enabled` says "enabled", but systemd does not start them at boot. Put them in the LOWER layer instead:
`mount --bind / /mnt/rb` (non-recursive, shows the underlying /etc), copy the unit to `/mnt/rb/etc/systemd/system/` and symlink `../<unit>` in `/mnt/rb/etc/systemd/system/multi-user.target.wants/`, `umount /mnt/rb`.
q2-headtrack and q2-buttons were already installed that way. Units: `rootfs-patches/camera/q2-camd.service`, `q2-ctltrack.service`, `rootfs-patches/radios/q2-radios.service`.
Source-built binaries in /usr/bin: q2camlive (patched, see tools/q2camlive-mask-init.patch), q2blobs, q2ctltrack (upstream sources, unmodified); originals saved as `*.upstream-prebuilt`.

### Controller tracking status (updated 2026-11-25)
**Native camera tracking works end to end, no Meta runtime involved.** With both controllers held 30-60 cm in front of the headset, `q2ctltrack` locks both hands: e.g. LEFT at x 0.27 y 0.03 z -0.66 m
from 17 LEDs seen by cameras 0x14, 0.84 px reprojection error; RIGHT found in the same window. SteamVR's q2hmd picks it up per hand ("left hand: position from the cameras", "controller orientation: controller IMU,
heading from the cameras") and falls back to the arm model after 1.5 s without camera data ("cameras lost it for 1.5 s, arm model"), so running q2-ctltrack at boot is safe. SteamVR + 4 cameras + solver: 49 C, fan 9.4k RPM.
- Camera order is right as is: `/var/lib/q2/cam_mirror` = 1 (mirrored slot -> calibration mapping) gave NO lock; leave the file absent.
- No LED sync was needed: the controllers' LEDs are visible and lock without us sending any NICE schedule or LED config.
- Weak point is lock *stability*: frames are very dark (max ~40/255 indoors, default exposure) and the solver loses hands often ("few dots"); in a 40 s window with the controllers held up the left locked in 6 of 40 one-second
  windows, the right in 3 (hand placement was not controlled). More exposure/gain, i.e. a NICE schedule, is the lever.

What is decoded of Meta's NICE (camera scheduler) protocol (2026-11-25; libsyncboss.so static analysis + the MCU's own log strings and replies as the oracle):
- **Framing:** the MCU's other "commands" are registers of the generic **SET_DATA (type 3)** message: `{0x03, seq, len, reg, payload...}`. Sending 0x27/0x49/0xef/0x64 as raw message types is rejected with status `0x0102`
  (that is also what the MCU console, reg 0x27 "shell", returns: it is disabled in this production firmware build, so no text console). Camera start/stop/init/deinit (0x2c/0x2d/0x2e/0x2f) ARE raw types.
- **Schedule = SET_DATA reg 0xc7**, one 147-byte block per camera set, then **SET_DATA reg 0x64** (no payload) to commit; the MCU applies it at camera start. Block: `[0]=camera set id, [1]=n slots (<=8), [2]=n sets,
  [3 + 17*i]= slot i, [139:143]=u32 frame period us (8333 = the NICE tick, firmware rounds to 8332), rest zero`. Slot (17 bytes, little endian): `u8 tag, u32 PERIOD us, u32 0, u32 CAPABILITY mask, u16 gain, u16 exposure us (<0x800)`.
  Capability bits (firmware print order): 0 Depth, 1 Constellation, 2 Hand LED, 3 **Unused** (setting it, e.g. mask 0x0f, makes the MCU assert "No slots available to schedule without the unused capability set"), 4 Droppable, 5 ETFT, 6 Eye LED, 7 Eye2 LED, 8 Face LED, 9 Body LED.
- **Valid camera sets are 1 and 3** ("No NICE handles found with camera_set 0 / 2" otherwise). Constraints reported by the MCU: each slot period >= 9139 us (independent of exposure), and the SUM of slot periods must be a multiple of the 8332 us tick
  ("Composite period 9266us is not aligned on nice period 8332us"), so at 60 Hz only a single 16664 us slot is possible at full resolution (Meta's table pairs 9266+7400 and 5200+11466 are for other frame configs). Exposure above ~500-1000 us makes the slot "unused" (assert) - 500 us accepted, 1000 us rejected; gain 128 accepted.
- Library helper `syncboss_nice_get_exposure_range` = GET_DATA-style reg 0x49 (request 0x0c + camera id): returns 0x0102 here. Table (type -> period class, slot period): 1: 8333/9266, 2: 8333/7400, 3: 16666/5200, 4: 16666/11466.
- Not decoded: controller LED config (`syncboss_input_set_led_config`, `..._set_led_ontime_us`, command-queue layer at 0x5aca0/0x68ca4), per-camera exposure/gain registers, the frame config (reg 0xb1) that sets the minimum slot period.
- **Result of the A/B controller test (controllers held steady ~40 cm in front, 60 s each):** default MCU schedule: LEFT locked 14 s, RIGHT 18 s (196 LED frames/s, 1.2 search tries/s); my single 16664 us slot with Depth|Constellation, exposure 500, gain 16:
  LEFT 0 s, RIGHT 0 s (0.1 tries/s). The schedule is accepted but makes tracking WORSE, and brightness (mean ~3-4/255, dominated by the sensor black level in a dark room) did not change with gain 128. So the NICE switch (`/var/lib/q2/nice`,
  read by q2camlive, off by default) stays OFF. In real VR use the hands locked only ~3% of the time, versus 23-30% when held still in front of the cameras, so camera field of view / hand placement during play is probably the bigger problem.
- Tools: `tools/sbproxy_nice.py` (stand-in forwarder that injects extra frames before camera start), `tools/nicetest.sh`, `tools/sbshell.py` (shell attempt, rejected), `tools/abtest.py` (lock statistics), `tools/pgmstats.py`.

### Boot bundle (2026-11-25)
`/mnt/HDD/quest2-boot/` (not in git) holds everything the Horizon-side jump needs: `Image-linux72` (byte-identical to the 7.2 build that ran all of 2026-11-25), `initramfs-linux72.cpio.gz` (stage 1: the repo's
`linux72/stage1/init` + busybox; pushed as `initramfs-probe.cpio.gz` because kxc.sh expects that name), `quest_kexec.ko`, `q2kexec`, `dtb/boot-{100,130,140,160,180}-fan.dtb`, `kxc.sh`, `host/` (nbd_server.py, kmsg_udp_recv.py, tn.py,
make_q2kexec_dtb.py) and `MANIFEST.sha256`. The DTBs are generated by `tools/make_claim_dtbs.sh <kexec dtb> <Image> <initramfs> <outdir> fan` and bake in the initramfs address, so regenerate them whenever the Image or the
initramfs changes. `tools/boot_quest2.sh [--go]` automates the whole Horizon -> Stage 2 sequence; see `BOOT_NEXT_TIME.md`. The rootfs itself is `/mnt/HDD/q2steamos/disk.img` (btrfs, generation 1226 at the last shutdown).
Clean shutdown: `q2vr stop`, `systemctl stop q2-ctltrack q2-camd` (deinitializes the MCU cameras), `sync`, `systemctl poweroff`; the headset then sits in its stock charging identity on USB (2833:0083, no adb).

