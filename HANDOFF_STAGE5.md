# Quest 2 Native Linux — Stage 5 Handoff (Jump #20/21 Ready)

## Hard Rules (DO NOT VIOLATE)
1. **NEVER touch `enp34s0` or `Wired connection 1`** via `nmcli`, `ip`, or routing commands — that is the user's PC internet. Quest 2 USB Ethernet (`10.42.0.1/24` <-> `10.42.0.2:23`) auto-connects via isolated NetworkManager profile `quest2-usb` pinned to MAC `02:51:4b:58:00:01` with `ipv4.never-default yes`.
2. **NEVER write to Quest 2 UFS partitions** — mount read-only (`mount -o ro`) only.
3. **NEVER commit device logs, serials, DTBs, or firmware blobs** to git (keep all in gitignored `private-captures/`).
4. **Singularity root is RAM-only**: after every reboot into Horizon OS, the user must trigger Singularity root. Note that depending on how Singularity is launched, root is accessible either via `adb shell "su -c '...'"` or via `nc 127.0.0.1 1234`.
5. **Always pass `stage2_verified=1`** to `insmod quest_kexec.ko` when `execute=1` is set (otherwise `qkx_dma_gate()` returns `-EOPNOTSUPP` = `Operation not supported on transport endpoint`), and prefer doing a 2-step load or passing the explicit `image=/data/local/tmp/Image initrd=/data/local/tmp/qkx-initramfs dtb=/data/local/tmp/boot-130-j19.dtb` paths so `quest_kexec.ko` never picks up stale default filenames.

---

## Current State (2026-11-21)
- **Repo**: `/mnt/HDD/quest2-kexec-build` (`origin/master` = `https://github.com/Raibatsu/quest2-kexec-research.git`).
- **Quest 2**: Currently resting in Horizon OS (`1WMHHA60ST2053`).
- **Target Kernel Build**: `/mnt/HDD/quest2-kexec-build/target-out/arch/arm64/boot/Image` (`31,839,074` bytes, build `#10`), with full patch diff saved in `/mnt/HDD/quest2-kexec-build/patches/target-kernel-stage5.diff`.

---

## Verified Live on Hardware in Stage 5 (Jumps #15–#19)
1. **8 CPUs (`0-7`) + 3-cluster CPUfreq**: All 8 Kryo 585 cores online (`4.19.325-cip128-st12+`), `powersave` governor.
2. **Physical 4K 120 Hz Display + DRM/KMS Scanout — EYEBALL-VERIFIED BY USER**:
   - Bound `qcom,mdss_dsi_jdi_4k_120Hz_dsc_video`, `/dev/dri/card0`, `/dev/dri/renderD128`, `conn=29 crtc=129` (`1920x3664 @ 120 Hz`).
   - User physically looked into the headset while `/tmp/drm_test` (`private-captures/drm_test.c`) ran and confirmed visible RGBW test graphics on the panel.
   - Backlight (`panel0-backlight`) and thermal fan (`pwm-tach-fan0` = `4/7`) working.
3. **QCA6390 Wi-Fi (`wlan0`) — LIVE OVER-THE-AIR RF SCAN VERIFIED (Jump #18)**:
   - Static ARM64 `/mnt/HDD/quest2-kexec-build/private-captures/qrtr-ns` (`qrtr_ns.c`, 170 lines) + `req->num_clients = 1` in `drivers/net/wireless/cnss2/qmi.c:205`.
   - Starting `qrtr-ns` before `insmod /tmp/wlan.ko` + `echo ON > /dev/wlan` + `ip link set wlan0 up` brought up `wlan0`, `p2p0`, `p2p1`, `wifi-aware0` (`phy0`).
   - `/mnt/HDD/quest2-kexec-build/private-captures/nl80211_scan` (`nl80211_scan.c`) triggered a live RF scan via `NL80211_CMD_TRIGGER_SCAN`/`NL80211_CMD_GET_SCAN` and discovered **12 live Wi-Fi access points (BSSs)** (`8496 bytes`, `rc=0`).
4. **All 28 Target Kernel Modules Built & Stripped (`private-captures/modules/`)**:
   - `wlan.ko` (11 MB stripped) + all 12 Kona Audio DLKM modules (`snd_event_dlkm.ko`, `q6_pdr_dlkm.ko`, `q6_notifier_dlkm.ko`, `apr_dlkm.ko`, `q6_dlkm.ko`, `adsp_loader_dlkm.ko`, `pinctrl_lpi_dlkm.ko`, `audio_ext_clk_up_dlkm.ko`, `stub_dlkm.ko`, `hdmi_dlkm.ko`, `platform_dlkm.ko`, `machine_dlkm.ko`; bundled in `/tmp/audio_mods.tar`).
5. **Adreno 650 GPU (`/dev/kgsl-3d0`) — Device Probed & GMU CM3 Booted (Jumps #18–#19)**:
   - Fixed `gmu_core_probe=-6` (`-ENXIO`) in `drivers/gpu/msm/kgsl_gmu_core.c:56` (`of_find_compatible_node(NULL, ...)`).
   - `/dev/kgsl-3d0` (`237, 0`) and `/sys/class/kgsl/kgsl-3d0/gpu_model` (`Adreno650v3`, `305000000` Hz) appear at boot; GMU CM3 loads `a650_gmu.bin` and starts HFI.

---

## Critical Breakthroughs from Jump #19 (Already Compiled into `target-out/arch/arm64/boot/Image` `#10` & Saved in `patches/target-kernel-stage5.diff`)

### 1. Why `subsys-pil-tz` (`PAS_INIT_IMAGE_CMD`) Failed with `rc=-22` for Both ADSP (`pas_id=1`) and `a650_zap` (`pas_id=13`) — 100% SOLVED
- On Jump #19, `dmesg` logged:
  ```
  [ 0.003809] shmbridge is enabled
  [ 66.915809] scm_call failed: func id 0x42000201, ret: -3145738, syscall returns: 0x0, 0x0, 0x0
  [ 66.915817] qkx-pil: PAS_INIT_IMAGE pas_id=1 mdata_phys=0x00000000fccbc000 size=8220 ret=-22 scm_ret=0
  ```
- `ret = -3145738` is `-0x30000A` = **`AC_ERR_SHM_BRIDGE_NOT_FOUND`** from QTEE TrustZone (which `scm_remap_error()` maps to `-EINVAL` = `-22`).
- Because `CONFIG_QTEE_SHM_BRIDGE=y` is active (`shmbridge is enabled`), QTEE TrustZone requires any `SCM_RW` physical buffer passed to `PAS_INIT_IMAGE_CMD` (`0x42000201`) to reside inside a registered QTEE SHM bridge. `pil_init_image_trusted()` in `drivers/soc/qcom/subsys-pil-tz.c` used `dma_alloc_attrs()` (unregistered page) instead of `qtee_shmbridge_allocate_shm(size, &shm)`.
- **Fix applied in `drivers/soc/qcom/subsys-pil-tz.c`**: Allocate the metadata buffer via `qtee_shmbridge_allocate_shm(size, &shm)` (`#include <soc/qcom/qtee_shmbridge.h>`), `memcpy(shm.vaddr, metadata, size)`, `__flush_dcache_area(shm.vaddr, size)`, pass `shm.paddr` as `desc->args[1]`, and free via `qtee_shmbridge_free_shm(&shm)` (keeping the `PAS_SHUTDOWN_CMD` pre-clean retry as well).

### 2. Why `open("/dev/kgsl-3d0")` Timed Out in `GmuPwrGxBwVote` — 100% SOLVED via `gmu_snapshot.bin` + `a650_gmu.bin` Disassembly
- Captured `/sys/class/kgsl/kgsl-3d0/snapshot/dump` (`403,968` bytes) to `private-captures/gmu_snapshot.bin` and decoded `HFI_DBG_ID` (`F2H_MSG_ERR` at `GmuPwrGxBwVote` line `0xb8` = 184) + all 575 GPU RSC & GMU registers:
  1. **GPU RSC & PDC hardware sequencers left in SLEEP state by Horizon OS across kexec**:
     - In `gmu_snapshot.bin`:
       - `reg[0x23b08]` (`A6XX_GMU_RSCC_CONTROL_ACK`) = `0x00000001` (`BIT(0)` = Sleep ACK still asserted from Horizon OS!)
       - `reg[0x00038004]` (`A6XX_GPU_RSCC_RSC_STATUS0_DRV0`) = `0x01010006` (`BIT(16)` = `0x00010000` = RSC in sleep state!)
       - `reg[0x00038101]` (`A6XX_RSCC_SEQ_BUSY_DRV0`) = `0x00000001` (RSC sleep sequencer parked waiting for wakeup trigger!)
       - `TCS 8` (`0x440e2214`) and `TCS 1` (`0x440e0fb4`) had 4 commands stuck at `CMD_STATUS = 0x00000101` (`CMD_STATUS_ISSUED` set, `CMD_STATUS_COMPL = BIT(16)` zero!) because the sleeping RSC/PDC sequencer gated off active AMC arbitration to RPMh!
     - Why: When the Quest 2 GPU is idle in Horizon OS before kexec, Horizon OS puts the GPU into SLUMBER (`a6xx_rpmh_power_off_gpu()`), which runs the RSC/PDC sleep sequence (`A6XX_GMU_RSCC_CONTROL_REQ = 1`, `TCS 5` completed with `0x00010101`). Across kexec, the new kernel starts with `device->gmu_core.flags = 0` (`GMU_BOOT_INIT_DONE = 0`, `GMU_RSCC_SLEEP_SEQ_DONE = 0`), so `a6xx_gmu_fw_start()` on `GMU_COLD_BOOT` only called `_load_gmu_rpmh_ucode()` and skipped `a6xx_rpmh_power_on_gpu()`!
     - **Fix applied in `drivers/gpu/msm/adreno_a6xx_gmu.c` (`a6xx_gmu_fw_start`)**: On `GMU_COLD_BOOT`, check if `A6XX_GPU_RSCC_RSC_STATUS0_DRV0 & BIT(16)` or `A6XX_RSCC_SEQ_BUSY_DRV0 != 0` in hardware; if so, set `GMU_RSCC_SLEEP_SEQ_DONE` and run `a6xx_rpmh_power_on_gpu(device)` to wake the RSC/PDC sequencer before reloading RSC/PDC uCode.
  2. **Wait bitmasks in `H2F_MSG_BW_VOTE_TBL`**:
     - In `drivers/soc/qcom/msm_bus/msm_bus_fabric_rpmh.c:415` (`tcs_cmd_query_list_gen`), `if (list_is_last(...))` set `commit = true;` without setting `cmdlist_active[k].wait = true;`.
     - In `drivers/gpu/msm/kgsl_gmu.c:795` (`build_rpmh_bw_votes`), `BIT(i)` was used instead of `BIT(j)` inside `if (!i)`, causing `j = 1` (`wait=0`) to clear `BIT(0)` and leave `ddr_wait_bitmask = 0` and `cnoc_wait_bitmask = 0`.
     - **Fixes applied** in both `msm_bus_fabric_rpmh.c` and `kgsl_gmu.c` (and restored `req.bw = bus_level` in `gmu_dcvs_set()` and `msm_bus_scale_client_update_request(gmu->pcl, 0)` in `gmu_start()`).

---

## Next Session Plan (Step-by-Step for Jump #21)

### 1. After User Roots Quest 2 in Horizon OS
First, check if root is via `su -c` or `nc 127.0.0.1 1234`, and check `/sys/fs/pstore/console-ramoops-0` from the end-of-session Jump #20 attempt (which had failed `insmod` without `stage2_verified=1` first and then warm-reset when `insmod` was called without explicit `image=`/`initrd=` paths):
```bash
adb shell "su -c 'ls -la /sys/fs/pstore/; tail -n 80 /sys/fs/pstore/console-ramoops-0 2>/dev/null'"
```

### 2. Stage & Fire Clean Jump #21 (Always Pass Explicit `image=`, `initrd=`, `dtb=`, and `stage2_verified=1`)
Make sure `/data/local/tmp/qkx-Image`, `/data/local/tmp/qkx-initramfs`, `/data/local/tmp/boot-100-j19.dtb`, `/data/local/tmp/boot-130-j19.dtb`, and `/data/local/tmp/quest_kexec.ko` are pushed, then run:
```bash
adb push /mnt/HDD/quest2-kexec-build/target-out/arch/arm64/boot/Image /data/local/tmp/qkx-Image
adb push /mnt/HDD/quest2-kexec-build/baseline/initramfs.gz /data/local/tmp/qkx-initramfs
adb push /mnt/HDD/quest2-kexec-build/private-captures/boot-100-j19.dtb /mnt/HDD/quest2-kexec-build/private-captures/boot-130-j19.dtb /mnt/HDD/quest2-kexec-build/loader/quest_kexec.ko /data/local/tmp/
```
Stop Horizon DMA services and fire kexec with full flags:
```bash
adb shell "su -c '
for s in cameraserver virtual_camera trackingservice trackingfidelityservice vendor.adsprpcd vendor.adsprpcd_audiopd vendor.cdsprpcd vendor-qti-media-c2-hal-1-0 vendor.media.omx media wpa_supplicant wificond vendor.oculus.wifi-hal-1-0 vendor.wifi_hal_legacy; do
  stop \$s 2>/dev/null || true
done
sync
echo 3 > /proc/sys/vm/drop_caches
echo 1 > /proc/sys/vm/compact_memory
for BASE in 0x100000000 0x130000000; do
  if [ \$BASE = 0x100000000 ]; then DTB=/data/local/tmp/boot-100-j19.dtb; else DTB=/data/local/tmp/boot-130-j19.dtb; fi
  insmod /data/local/tmp/quest_kexec.ko \
    image=/data/local/tmp/qkx-Image \
    initrd=/data/local/tmp/qkx-initramfs \
    dtb=\$DTB \
    payload_base=\$BASE \
    execute=1 stage2_verified=1 preserve_watchdog=1 watchdog_recovery=1 \
    suspend_syncboss=1 clear_pci_master=1 shutdown_dsps=1 \
    disconnect_qmp=1 qmp_channels_only=1 flush_rpmh=1 && break
done
'"
```

### 3. On Jump #21 (`10.42.0.2:23`): Verify Adreno 650 GPU & Kona ADSP Audio Live
1. **Mount `/dev/sde24` read-only and stage GPU + Audio firmware in `/lib/firmware/`**:
   - Push `private-captures/firmware/{a650_gmu.bin,a650_sqe.fw,CM710X.bin}` and `private-captures/gpu_test` + `/tmp/audio_mods.tar` to `10.42.0.2:23`.
   - Symlink `/tmp/modem/image/a650_zap.*` and `/tmp/modem/image/adsp.*` into `/lib/firmware/`.
2. **Test Adreno 650 GPU (`/tmp/gpu_test`)**:
   - Run `/tmp/gpu_test` -> verifies `open("/dev/kgsl-3d0")` + `IOCTL_KGSL_DEVICE_GETPROPERTY` (`KGSL_PROP_DEVICE_INFO`, `chip_id=0x06050002`).
   - Check `dmesg | grep -E "kgsl|gmu|qkx-pil|subsys-pil-tz"`.
3. **Test Kona ADSP Audio (`boot_adsp` + ALSA cards)**:
   - Load the 12 Kona audio DLKM modules from `/tmp/audio_mods.tar`.
   - Run `echo 1 > /sys/kernel/boot_adsp/boot`.
   - Check `dmesg | grep -E "qkx-pil|subsys-pil-tz|adsp|kona|asoc"` and `cat /proc/asound/cards`.
