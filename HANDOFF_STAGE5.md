# Quest 2 Native Linux — Stage 5 Handoff (Jump #22 Ready)

## Hard Rules (DO NOT VIOLATE)
1. **NEVER touch `enp34s0` or `Wired connection 1`** via `nmcli`, `ip`, or routing commands — that is the user's PC internet. Quest 2 USB Ethernet (`10.42.0.1/24` <-> `10.42.0.2:23`) auto-connects via isolated NetworkManager profile `quest2-usb` pinned to MAC `02:51:4b:58:00:01` with `ipv4.never-default yes`.
2. **NEVER write to Quest 2 UFS partitions** — mount read-only (`mount -o ro`) only.
3. **NEVER commit device logs, serials, DTBs, or firmware blobs** to git (keep all in gitignored `private-captures/`).
4. **Singularity root is RAM-only**: after every reboot into Horizon OS, the user must trigger Singularity root. Depending on how Singularity is launched, root is accessible either via `adb shell "su -c '...'"` or via `nc 127.0.0.1 1234`.
5. **Always pass `stage2_verified=1`** to `insmod quest_kexec.ko` when `execute=1` is set (otherwise `qkx_dma_gate()` returns `-EOPNOTSUPP` = `Operation not supported on transport endpoint`), and always pass explicit `image=... initrd=... dtb=...` paths so `quest_kexec.ko` never picks up stale default filenames.
6. **Always pass `preserve_watchdog=1` (alongside `watchdog_recovery=1`)**: Confirmed via Jump #20 `/sys/fs/pstore/console-ramoops-0` — omitting `preserve_watchdog=1` triggers `Kernel panic - not syncing: quest_kexec: watchdog disable failed after USB shutdown (-5)` and warm-resets back to Horizon OS.

---

## Current State (2026-11-21)
- **Repo**: `/mnt/HDD/quest2-kexec-build` (`origin/master` = `https://github.com/Raibatsu/quest2-kexec-research.git`).
- **Quest 2**: Reset back to Horizon OS at the end of Jump #21 right after completing the live QCA6390 Bluetooth UART EDL probe, then disconnected from USB; paused here for sleep.
- **Target Kernel Build**: `/mnt/HDD/quest2-kexec-build/target-out/arch/arm64/boot/Image` (`31,839,074` bytes, build `#10`), with full patch diff saved in `/mnt/HDD/quest2-kexec-build/patches/target-kernel-stage5.diff` (untouched since Jump #21).
- **Pre-Built Test Payloads in `private-captures/` (Ready for Jump #22)**:
  - `/mnt/HDD/quest2-kexec-build/private-captures/bt_hci_test` (static ARM64 binary from `bt_hci_test.c`).
  - `/mnt/HDD/quest2-kexec-build/private-captures/pil_probe/pil_probe.ko` (built cleanly against `target-out` `#10` from `pil_probe.c`).

---

## Verified Live on Hardware in Stage 5 (Jumps #15–#21)
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
5. **Adreno 650 GPU (`/dev/kgsl-3d0`) — GMU CM3, GPU RSC Wakeup, HFI, `GmuPwrGxBwVote`, SQE, AND `CP_ME_INIT` Ringbuffer Execution VERIFIED LIVE (Jump #21)**:
   - Fixed `gmu_core_probe=-6` (`-ENXIO`) in `drivers/gpu/msm/kgsl_gmu_core.c:56` (`of_find_compatible_node(NULL, ...)`).
   - Fixed Horizon OS SLUMBER / GPU RSC sleep-state carry-over across kexec in `drivers/gpu/msm/adreno_a6xx_gmu.c` (`a6xx_gmu_fw_start()`: detect `A6XX_GPU_RSCC_RSC_STATUS0_DRV0 & BIT(16)` or `A6XX_RSCC_SEQ_BUSY_DRV0 != 0` on `GMU_COLD_BOOT`, set `GMU_RSCC_SLEEP_SEQ_DONE`, and run `a6xx_rpmh_power_on_gpu(device)`), plus wait bitmask fixes in `drivers/soc/qcom/msm_bus/msm_bus_fabric_rpmh.c:415` and `drivers/gpu/msm/kgsl_gmu.c:795`.
   - **Verified live on Jump #21 (`#10`)**: Opening `/dev/kgsl-3d0` via `/tmp/gpu_test` succeeded through `gmu_start()`, `hfi_start()`, `GmuPwrGxBwVote` (`kgsl_pwrctrl_set_default_gpu_pwrlevel`), CP SQE start (`a650_sqe.fw`), and **`a6xx_send_cp_init()` (`CP_ME_INIT` ringbuffer submission and idle completion)** with zero GMU/HFI/CP timeouts.
   - `open("/dev/kgsl-3d0")` returned `-EINVAL` (`-22`) only on the trailing `a6xx_microcode_load()` -> `adreno_zap_shader_load(adreno_dev, "a650_zap")` call (`PAS_INIT_IMAGE_CMD` `ret = -3145738`). Note that `drivers/gpu/msm/adreno_a6xx.c:902-913` already includes the non-zap fallback `if (!adreno_dev->zap_loaded) kgsl_regwrite(device, A6XX_RBBM_SECVID_TRUST_CNTL, 0);`.
6. **QCA6390 Bluetooth (`/dev/ttyHS0` + `rfkill0`) — LIVE UART HCI HANDSHAKE VERIFIED (Jump #21)**:
   - Serial nodes present: `/dev/ttyHS0` (`998000.qcom,qup_uart`), `/dev/ttyHS13`, `/dev/ttyHS14`.
   - `echo 1 > /sys/class/rfkill/rfkill0/state` (`bt_power`) enabled regulators `qca,bt-vdd-aon`, `qca,bt-vdd-dig`, `qca,bt-vdd-rfa1`, `qca,bt-vdd-rfa2`, and asserted `bt-reset-gpio(1121)` = `1` and `bt-sw-ctrl-gpio(1224)` = `1`.
   - After cycling `rfkill0` (`0 -> 0.3s -> 1`), `/tmp/bt_hci_test` (`private-captures/bt_hci_test.c`) over `/dev/ttyHS0` at `115200 8N1 CRTSCTS=1` sent `EDL_PATCH_VER_REQ` (`01 00 fc 01 19`) and received the **21-byte QCA6390 ROM bootloader response**:
     `04 0e 12 01 00 fc 00 19 0c 10 00 00 00 2b 0d 00 02 00 02 0a 40` (Command Complete status `0x00`, Product ID `0x0000100C` = QCA6390/Hastings, SoC/ROM v2.0).
   - Note: Right after receiving those 21 bytes (during `HCI_RESET` / `tcdrain`, following the `rfkill0` cycle where `bt_enable_bt_reset_gpios_safely` toggled `wl-reset-gpio(1120)` while `wlan.ko` was loaded), the Quest 2 reset back to Horizon OS. Inspect `/sys/fs/pstore/console-ramoops-0` on next root to confirm the reset cause. Also note `target-out/.config` currently has `CONFIG_BT=y`, `CONFIG_BT_BREDR=y`, `CONFIG_BT_HS=y`, `CONFIG_BT_LE=y`, `CONFIG_BTFM_SLIM=y`, `CONFIG_BT_SLIM_QCA6390=y`, while `CONFIG_BT_HCIUART`, `CONFIG_BT_RFCOMM`, `CONFIG_BT_BNEP`, and `CONFIG_BT_HIDP` are still unset (needed for full native BlueZ over `/dev/ttyHS0`).

---

## Jump #21 Investigation: Why `PAS_INIT_IMAGE_CMD` (`0x42000201`) Returns `-3145738` (`-0x30000A`)
- Earlier hypothesis (`AC_ERR_SHM_BRIDGE_NOT_FOUND` for `dma_alloc_attrs`) was **disproved live on Jump #21**:
  - Even with `qtee_shmbridge_allocate_shm()` (`mdata_phys = 0x1f8400000` inside `default_bridge`), `PAS_INIT_IMAGE_CMD` (`0x42000201`) still returned `ret: -3145738` (`-0x30000A` -> `-EINVAL` `-22`) for both `adsp` (`pas_id=1`) and `a650_zap` (`pas_id=13`), while pre-clean `PAS_SHUTDOWN_CMD` (`pas_id=1`) returned `0`.
- Disassembly of `/dev/sde23` (`hyp_b.elf`) showed EL2 intercepts `PAS_INIT_IMAGE_CMD`:
  - Entry wrapper at `0x800573a4` calls `pil_init_image_api_ns` (`0x800561ec`) and negates non-negative error codes via `cneg w0, w0, ge` (`0x800573b4`) — producing `-0x30000A` (`-3145738`) from positive `0x30000A`.
  - `pil_init_image_api_ns` calls `PIL_parseMetadata` (`0x800557e8`, calling `PILSourceNS_getData` / ELF parser `0x80071f64`, emitting `0x30000A` = `0x30005b - 0x51` when ELF header/source access fails) and `protect_metadata` (`0x80056cd8` / `0x80056958` -> `0x80056754`), which calls `ACMapMemory` (`0x80065a08`) to assign the metadata buffer `[mdata_phys, ALIGN(size, 4096)]` from `VMID_HLOS` (`3`) to `VMID_HYP` (`4`) with `perm = 6` (`PERM_READ | PERM_WRITE`).
  - Why both attempts failed:
    1. `qtee_shmbridge_allocate_shm()` (`default_bridge` at `0x1f8400000`) uses shared SHM-bridge memory (not exclusively owned by `VMID_HLOS`), which is incompatible with `protect_metadata` (`VMID 3 -> VMID 4`).
    2. Stock `dma_alloc_attrs(..., DMA_ATTR_STRONGLY_ORDERED)` on Jump #19 allocated `0xfccbc000` inside `linux,cma` (`0xfcc00000..0xfebfffff`), which may retain stale non-HLOS VMID ownership from Horizon OS across kexec.
    3. Also note `md5sum /dev/sde23 /dev/sde40` showed `hyp_xros_b` (`/dev/sde40`) differs from `hyp_b` (`/dev/sde23`).
- **Pre-built RAM probe ready for Jump #22**:
  `/mnt/HDD/quest2-kexec-build/private-captures/pil_probe/pil_probe.ko` tests:
  1. Clean buddy pages (`__get_free_pages` + `__flush_dcache_area`) for `adsp.mdt` (`pas_id=1`).
  2. Direct `hyp_assign_phys(VMID_HLOS -> VMID_HYP -> VMID_HLOS)` ownership checks on buddy pages.
  3. Stock `dma_alloc_attrs(..., DMA_ATTR_STRONGLY_ORDERED)` `hyp_assign_phys` + `PAS_INIT_IMAGE_CMD`.
  4. Buddy pages for `a650_zap.mdt` (`pas_id=13`).

---

## Next Session Plan (Step-by-Step After Sleep)

### 1. After Rooting Quest 2 in Horizon OS
First check `/sys/fs/pstore/console-ramoops-0` to inspect why the Quest 2 reset during the end of Jump #21 (`rfkill0` cycle / `bt_hci_test`), then push/fire Jump #22 using the already-built `Image` `#10`:
```bash
adb shell "su -c 'ls -la /sys/fs/pstore/; tail -n 80 /sys/fs/pstore/console-ramoops-0 2>/dev/null'"
```
Fire clean Jump #22 (`stage2_verified=1 preserve_watchdog=1 watchdog_recovery=1` + explicit paths):
```bash
adb push /mnt/HDD/quest2-kexec-build/target-out/arch/arm64/boot/Image /data/local/tmp/qkx-Image
adb push /mnt/HDD/quest2-kexec-build/baseline/initramfs.gz /data/local/tmp/qkx-initramfs
adb push /mnt/HDD/quest2-kexec-build/private-captures/boot-100-j19.dtb /mnt/HDD/quest2-kexec-build/private-captures/boot-130-j19.dtb /mnt/HDD/quest2-kexec-build/loader/quest_kexec.ko /data/local/tmp/
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

### 2. Run `pil_probe.ko` Live on Jump #22 (`10.42.0.2:23`)
Mount `/dev/sde24` read-only at `/tmp/modem`, symlink `adsp.*` and `a650_zap.*` into `/lib/firmware/`, upload `/mnt/HDD/quest2-kexec-build/private-captures/pil_probe/pil_probe.ko` to `/tmp/pil_probe.ko`, run `insmod /tmp/pil_probe.ko` (intentionally returns `-EAGAIN`), and check `dmesg | grep -E "pil_probe|scm_call|hyp_assign"`.
- If Test 1/Test 4 (`__get_free_pages` + `__flush_dcache_area`) succeeds (`ret=0 scm_ret=0`): update `pil_init_image_trusted()` in `drivers/soc/qcom/subsys-pil-tz.c` to use `__get_free_pages`, enable `CONFIG_BT_HCIUART`, `CONFIG_BT_HCIUART_H4`, `CONFIG_BT_HCIUART_QCA`, `CONFIG_BT_RFCOMM`, `CONFIG_BT_BNEP`, `CONFIG_BT_HIDP` in `target-out/.config`, rebuild `Image` `#11`, and verify end-to-end `/dev/kgsl-3d0` (Adreno 650) + `/proc/asound/cards` (Kona ADSP Audio).
- If all tests still return `-3145738` even when `hyp_assign_phys(HLOS->HYP)` succeeds: pull `/dev/sde40` (`hyp_xros_b`) read-only to inspect XROS EL2 PIL metadata handling (and check whether `adreno_a6xx.c` can skip `a650_zap` via `A6XX_RBBM_SECVID_TRUST_CNTL` fallback).

### 3. Remaining Stage 5 / Dual-Boot Milestones After Step 1 Hardware Sign-Off
- **Step 2 — Linux Userspace Rootfs & GPU Userspace**: Minimal ARM64 Linux rootfs (Debian/Arch on USB/ext4/image) + Wayland/DRM compositor + Mesa Turnip/Freedreno against Adreno 650 + `btattach`/BlueZ + `wpa_supplicant`/`iwd` + ALSA/PipeWire.
- **Step 3 — XR / Peripherals**: `syncboss` / SPI controller/IMU/6DoF integration, OpenXR / Monado display distortion & audio routing.
- **Step 4 — SteamOS / Dual-Boot Packaging**: Clean initramfs + switchable userspace rootfs / SteamOS session launcher.
