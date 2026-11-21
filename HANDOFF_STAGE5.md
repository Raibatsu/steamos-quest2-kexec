# Quest 2 Native Linux — Stage 5 Handoff (Jump #17 Ready)

## Hard Rules (DO NOT VIOLATE)
1. **NEVER touch `enp34s0` or `Wired connection 1`** via `nmcli`, `ip`, or routing commands — that is the user's PC internet. Quest 2 USB Ethernet (`10.42.0.1/24` <-> `10.42.0.2:23`) auto-connects via isolated NetworkManager profile `quest2-usb` pinned to MAC `02:51:4b:58:00:01` with `ipv4.never-default yes`.
2. **NEVER write to Quest 2 UFS partitions** — mount read-only (`mount -o ro`) only.
3. **NEVER commit device logs, serials, DTBs, or firmware blobs** to git (keep all in gitignored `private-captures/`).
4. **Singularity root is RAM-only**: after every reboot into Horizon OS, the user must unplug USB -> tap Singularity "Root Now" -> replug USB. (Also note: the user's USB cable is slightly loose when moving the headset.)

## Current State (2026-11-21)
- **Repo**: `/mnt/HDD/quest2-kexec-build` (`origin/master` = `https://github.com/Raibatsu/quest2-kexec-research.git`).
- **Quest 2**: Just sent `reboot -f` from Jump #16 -> rebooting into Horizon OS.
- **Working live in Stage 5 (verified on Jumps #15–#16)**:
  - **8 CPUs (`0-7`)** online (`4.19.325-cip128-st12+`).
  - **Physical 4K 120 Hz Display + DRM/KMS**: `qcom,mdss_dsi_jdi_4k_120Hz_dsc_video`, `/dev/dri/card0`, `/dev/dri/renderD128`, `conn=29 crtc=129`, `1920x3664 @ 120 Hz`. **Eyeball-verified** live RGBW test graphics via `/mnt/HDD/quest2-kexec-build/private-captures/drm_test` (source: `drm_test.c`).
  - **Backlight & Thermal Fan**: `panel0-backlight`, `pwm-tach-fan0` dropped from 100% failsafe to `4/7`.
  - **UFS Storage**: `/dev/sde24` = `modem_b` (active newer slot, `Jul 8 2026`), `/dev/sde4` = `modem_a`.
  - **All 28 target kernel modules built** in `/mnt/HDD/quest2-kexec-build/target-out` and debug-stripped copies staged in `/mnt/HDD/quest2-kexec-build/private-captures/modules/`:
    - `wlan.ko` (11 MB stripped)
    - All 12 Kona audio DLKMs (`snd_event_dlkm.ko`, `q6_pdr_dlkm.ko`, `q6_notifier_dlkm.ko`, `apr_dlkm.ko`, `q6_dlkm.ko`, `adsp_loader_dlkm.ko`, `pinctrl_lpi_dlkm.ko`, `audio_ext_clk_up_dlkm.ko`, `stub_dlkm.ko`, `hdmi_dlkm.ko`, `platform_dlkm.ko`, `machine_dlkm.ko`)
  - **Pulled Horizon firmware blobs** (in `/mnt/HDD/quest2-kexec-build/private-captures/firmware/`):
    - GPU: `a650_gmu.bin`, `a650_sqe.fw`, `a650_zap.elf` (plus `a650_zap.{b00..b02,mdt}` on `/dev/sde24` `modem_b`)
    - Wi-Fi: `wlan/qca_cld/WCNSS_qcom_cfg.ini`, `bdwlan.elf` (plus `qca6390/{amss20.bin,m3.bin,Data.msc,regdb.bin,bdwlan.*}` on `/dev/sde24` `modem_b`)
    - Audio: `CM710X.bin` (plus `adsp.*` on `/dev/sde24` `modem_b`)
  - **Static ARM64 QRTR Nameserver**: `/mnt/HDD/quest2-kexec-build/private-captures/qrtr-ns` (source: `qrtr_ns.c`). Linux 4.19 has no in-kernel QRTR NS; with `qrtr-ns` running, QCA6390 (`node 7 port 1`) registers `WLFW_SERVICE_ID_V01` (`svc=69`) and `cnss2` connects **`QMI WLFW service connected`**!

## Target Kernel Patches Currently in `/mnt/HDD/quest2-kexec-build/kernel-target-src/`
1. `drivers/mailbox/msm_qmp.c`: warm-attach AOP in `LINK_CONNECTED` if `mcore`/`ucore` already `QMP_MBOX_LINK_UP`.
2. `drivers/soc/qcom/msm_bus/msm_bus_fabric_rpmh.c`: `defer_qos = true`.
3. `kernel/hung_task.c`: `sysctl_hung_task_timeout_secs = 15`.
4. `drivers/soc/qcom/watchdog_v2.c`: diagnostic `if (++qkx_pet_cnt >= 2 && system_state < SYSTEM_RUNNING)` panic (harmless once `/init` runs; clean up at end of Stage 5).
5. `drivers/gpu/msm/adreno.c`: 3 temporary `pr_info("qkx-adreno: ...")` lines added right before Jump #17 to log return codes of `adreno_of_get_power`, `gmu_core_probe`, and `kgsl_device_platform_probe`.

**Build command for `target-out`** (keep in mind Clang 23 needs `-Wno-strict-prototypes -Wno-implicit-enum-enum-cast -Wno-error -I$(srctree)/drivers/pinctrl` for `modules`):
```bash
make -C /mnt/HDD/quest2-kexec-build/kernel-target-src \
  O=/mnt/HDD/quest2-kexec-build/target-out \
  ARCH=arm64 CC=clang LD=ld.lld \
  CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
  -j$(nproc) Image </dev/null
```

## Exact Jump #17 Procedure (Once User Roots in Horizon OS)

### 1. Push freshly rebuilt `Image` (if size changed, verify `boot-130-stage5.dtb` initrd-start still >= `0x130000000 + ALIGN(text_offset + image_size, 4096)`)
- Note: `Image` was rebuilt with 3 `pr_info` lines in `adreno.c`. Push `/mnt/HDD/quest2-kexec-build/target-out/arch/arm64/boot/Image` -> `/data/local/tmp/qkx-Image` and run `adb shell sync`!
- Recompute `linux,initrd-start` / `linux,initrd-end` in DTB if `Image` `image_size` (header offset `0x10`, LE64) changed! (Always verify against `loader/loader.c` check: `initrd_start >= payload_base + ALIGN(text_offset + image_size, 4096)`.)
- Working `bootargs` (already in `private-captures/boot-130-stage5.dtb`):
  ```
  rdinit=/init loglevel=7 lpm_levels.sleep_disabled=1 msm_drm.dsi_display0=qcom,mdss_dsi_jdi_4k_120Hz_dsc_video: regulator_ignore_unused initcall_blacklist=qseecom_init
  ```
- Stop Horizon DMA services + fire kexec:
  ```bash
  for s in cameraserver virtual_camera trackingservice trackingfidelityservice vendor.adsprpcd vendor.adsprpcd_audiopd vendor.cdsprpcd vendor-qti-media-c2-hal-1-0 vendor.media.omx media wpa_supplicant wificond vendor.oculus.wifi-hal-1-0 vendor.wifi_hal_legacy; do
    stop $s 2>/dev/null || true
  done
  sleep 1
  insmod /data/local/tmp/quest_kexec.ko \
    image=/data/local/tmp/qkx-Image \
    initrd=/data/local/tmp/qkx-initramfs \
    dtb=/data/local/tmp/qkx-boot.dtb \
    payload_base=0x130000000 \
    execute=1 stage2_verified=1 preserve_watchdog=1 watchdog_recovery=1 \
    suspend_syncboss=1 clear_pci_master=1 shutdown_dsps=1 \
    disconnect_qmp=1 qmp_channels_only=1 flush_rpmh=1
  ```

### 2. Immediate Stage 5 Goals on Jump #17 (`10.42.0.2:23`)
1. **Wi-Fi (`wlan0`)**:
   - Mount `/dev/sde24` (`modem_b`) read-only at `/tmp/modem`.
   - Symlink `/tmp/modem/image/qca6390/*` into `/lib/firmware/qca6390/` and `/lib/firmware/wlan/qca6390/` (replace `bdwlan.elf` with pulled `private-captures/firmware/bdwlan.elf`, push `WCNSS_qcom_cfg.ini` to `/lib/firmware/wlan/qca_cld/`).
   - **START `/tmp/qrtr-ns >/tmp/qrtr.log 2>&1 &` FIRST!**
   - **THEN** `insmod /tmp/wlan.ko` (and if needed `echo ON > /dev/wlan`).
   - Check `ip link` for `wlan0` and `dmesg | grep -iE 'cnss|wlan|qmi'`!
   - Note: if `wlan.ko` vermagic complains because `Image` was rebuilt, rebuild `wlan.ko` or pass `--force` (actually `CONFIG_MODVERSIONS` / `LOCALVERSION` stays `4.19.325-cip128-st12+` — rebuild `wlan.ko` if `vermagic` mismatches).
2. **GPU (`/dev/kgsl-3d0`)**:
   - Check `dmesg | grep qkx-adreno` immediately on boot to see which of `adreno_of_get_power`, `gmu_core_probe`, or `kgsl_device_platform_probe` returned an error!
   - Fix that exact failure, stage `a650_gmu.bin` + `a650_sqe.fw` + `a650_zap.*` in `/lib/firmware/`, and verify `/dev/kgsl-3d0` appears.
3. **Audio (`ADSP` + `Kona ALSA`)**:
   - All 12 DLKMs already load cleanly (`rc=0`).
   - Investigate why `echo 1 > /sys/kernel/boot_adsp/boot` gave `subsys-pil-tz 17300000.qcom,lpass: adsp: Initializing image failed(rc:-22)` (`PAS_INIT_IMAGE` `-EINVAL` — check if `adsp.mdt` / `PAS_ID` needs `initcall_blacklist=qseecom_init` removed or if TZ already has ADSP state / memory region issue).

## Jump #17 Breakthroughs (BOTH Root Causes Found & Patched in `kernel-target-src`!)
1. **GPU (`/dev/kgsl-3d0`) — ROOT CAUSE FOUND & FIXED**:
   - Jump #17 printed `qkx-adreno: adreno_of_get_power=0`, `qkx-adreno: gmu_core_probe=-6` (`-ENXIO`).
   - Root cause: `dtc -I fs` sorted `live-fdt.dtb` alphabetically, placing `qcom,gmu@3d6a000` *before* `qcom,kgsl-3d0@3d00000`. `kgsl_gmu_core.c:56` called `of_find_compatible_node(device->pdev->dev.of_node, NULL, "qcom,gpu-gmu")`, which only searches *after* `kgsl-3d0` and returned `NULL` -> `-ENXIO`.
   - **Fix applied** in `kernel-target-src/drivers/gpu/msm/kgsl_gmu_core.c`: changed `device->pdev->dev.of_node` to `NULL` (search from root).
2. **Wi-Fi (`wlan0` / QCA6390) — ROOT CAUSE FOUND & FIXED**:
   - With `qrtr-ns` started *before* `insmod wlan.ko`, `cnss2` cleanly reached `QMI WLFW service connected` (`state: 0x11`, zero recovery!), sent `indication register` + `host capability`, and then timed out waiting for `FW_READY_IND` because `cnss2/qmi.c:207` sent `req->num_clients = 2` (`ENABLE_DAEMON_SUPPORT`), telling QCA6390 firmware to wait forever for Android's `cnss-daemon`!
   - **Fix applied** in `kernel-target-src/drivers/net/wireless/cnss2/qmi.c`: set `req->num_clients = 1` (no Android `cnss-daemon`).
3. **Next Step (Jump #18)**:
   - Rebuild `target-out/arch/arm64/boot/Image` (now includes BOTH `kgsl_gmu_core.c` GPU fix AND `cnss2/qmi.c` `num_clients=1` Wi-Fi fix!).
   - `reboot -f` from current Jump #17 (`10.42.0.2:23`) -> Horizon root -> push rebuilt `Image` + recomputed DTB -> fire Jump #18.
   - On Jump #18: check `/dev/kgsl-3d0` (GPU!) AND start `/tmp/qrtr-ns` -> `insmod /tmp/wlan.ko` -> `wlan0` (Wi-Fi!)!

## Jump #18 Verified Results & Jump #19 Patches (2026-11-21)
- **Verified on Jump #18 (`payload_base=0x100000000`)**:
  1. **GPU (`/dev/kgsl-3d0`)**: `gmu_core_probe=0`, `kgsl_device_platform_probe=0`, `/dev/kgsl-3d0` (`237, 0`) and `/sys/class/kgsl/kgsl-3d0/gpu_model` (`Adreno650v3`, `305000000` Hz) live! Opening `/dev/kgsl-3d0` booted GMU (`a650_gmu.bin`) and completed 8 HFI messages (`sequence 0..8`), timing out only at `sequence 9` (`H2F_MSG_GX_BW_PERF_VOTE` `perf idx 255, bw idx 3`) in `kgsl_bus_scale_request()`.
  2. **Wi-Fi (`wlan0`)**: `qrtr-ns` + `insmod /tmp/wlan.ko` + `echo ON > /dev/wlan` + `ip link set wlan0 up` brought up `wlan0`, `p2p0`, `p2p1`, `wifi-aware0` (`phy0`). `/tmp/nl80211_scan` (`private-captures/nl80211_scan.c`) scanned **12 live Wi-Fi access points (BSSs)** (`8496 bytes`, `rc=0`) over the air!
- **Patched for Jump #19 (saved in `patches/target-kernel-stage5.diff`)**:
  - `drivers/gpu/msm/kgsl_pwrctrl.c` & `kgsl_gmu.c`: scale GPU DDR BW via AP RPMh bus client (`KGSL_GMU_DEVICE(device)->pcl`) instead of GMU HFI BW voting.
  - `drivers/soc/qcom/subsys-pil-tz.c`: `GFP_DMA32` + `__flush_dcache_area` + `qkx-pil` logging + pre-clean `PAS_SHUTDOWN_CMD` retry if `PAS_INIT_IMAGE_CMD` fails (for `a650_zap` `pas_id=13` and `adsp` `pas_id=1`).
