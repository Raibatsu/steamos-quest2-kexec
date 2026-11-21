# Quest 2 Native Linux — Stage 5 Handoff (Jump #22 Ready)

## Hard Rules (DO NOT VIOLATE)
1. **NEVER touch `enp34s0` or `Wired connection 1`** via `nmcli`, `ip`, or routing commands — that is the user's PC internet. Quest 2 USB Ethernet (`10.42.0.1/24` <-> `10.42.0.2:23`) auto-connects via isolated NetworkManager profile `quest2-usb` pinned to MAC `02:51:4b:58:00:01` with `ipv4.never-default yes`.
2. **NEVER write to or flash Quest 2 UFS partitions** — mount/read (`mount -o ro`, `cat`) read-only only.
3. **NEVER commit device logs, serials, DTBs, or firmware blobs** to git (keep all in gitignored `private-captures/`).
4. **Singularity root is RAM-only**: after every reboot into Horizon OS, the user must trigger Singularity root. Depending on how Singularity is launched, root is accessible either via `adb shell "su -c '...'"` or via `nc 127.0.0.1 1234`.
5. **Always pass `stage2_verified=1`** to `insmod quest_kexec.ko` when `execute=1` is set (otherwise `qkx_dma_gate()` returns `-EOPNOTSUPP` = `Operation not supported on transport endpoint`), and always pass explicit `image=... initrd=... dtb=...` paths so `quest_kexec.ko` never picks up stale default filenames.
6. **Always pass `preserve_watchdog=1` (alongside `watchdog_recovery=1`)**: Confirmed via Jump #20 `/sys/fs/pstore/console-ramoops-0` — omitting `preserve_watchdog=1` triggers `Kernel panic - not syncing: quest_kexec: watchdog disable failed after USB shutdown (-5)` and warm-resets back to Horizon OS.

---

## Current State (2026-11-21)
- **Repo**: `/mnt/HDD/quest2-kexec-build` (`origin/master` = `https://github.com/Raibatsu/quest2-kexec-research.git`).
- **Quest 2**: Reset back to Horizon OS at the end of Jump #21; read-only Horizon OS research completed cleanly (nothing flashed/written); paused here for sleep.
- **Target Kernel Build**: `/mnt/HDD/quest2-kexec-build/target-out/arch/arm64/boot/Image` (`31,839,074` bytes, build `#10`), with full patch diff saved in `/mnt/HDD/quest2-kexec-build/patches/target-kernel-stage5.diff`.
- **Captured & Pre-Built Assets in Gitignored `private-captures/` (Ready for Jump #22)**:
  - `private-captures/horizon-raw-fdt.dtb` (`517,088` bytes — exact raw unsorted `/sys/firmware/fdt` captured read-only from rooted Horizon OS).
  - `private-captures/firmware/qca/htbtfw20.tlv` (`205,760` bytes) and `htnv20.bin` (`5,839` bytes) — QCA6390 v2.0 Bluetooth firmware & NVM captured read-only from `/vendor/bt_firmware/image/`.
  - `private-captures/bt_hci_test` (static ARM64 binary from `bt_hci_test.c`).
  - `private-captures/pil_probe/pil_probe.ko` (built cleanly against `target-out` `#10` from `pil_probe.c`).

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
   - **Verified live on Jump #21 (`#10`)**: Opening `/dev/kgsl-3d0` via `/tmp/gpu_test` succeeded through `gmu_start()`, `hfi_start()`, `GmuPwrGxBwVote` (`kgsl_pwrctrl_set_default_gpu_pwrlevel`), CP SQE start (`a650_sqe.fw`), and **`a6xx_send_cp_init()` (`CP_ME_INIT` ringbuffer submission and idle completion)** with zero GMU/HFI/CP timeouts, failing only on trailing `a6xx_microcode_load()` (`a650_zap`, `pas_id=13`). Note `drivers/gpu/msm/adreno_a6xx.c:902-913` already includes the non-zap fallback `if (!adreno_dev->zap_loaded) kgsl_regwrite(device, A6XX_RBBM_SECVID_TRUST_CNTL, 0);`.
6. **QCA6390 Bluetooth (`/dev/ttyHS0` + `rfkill0`) — LIVE UART HCI HANDSHAKE VERIFIED (Jump #21)**:
   - `echo 1 > /sys/class/rfkill/rfkill0/state` (`bt_power`) enabled `qca,bt-vdd-aon`, `qca,bt-vdd-dig`, `qca,bt-vdd-rfa1`, `qca,bt-vdd-rfa2`, and asserted `bt-reset-gpio(1121)` = `1` and `bt-sw-ctrl-gpio(1224)` = `1`.
   - After cycling `rfkill0` (`0 -> 0.3s -> 1`), `/tmp/bt_hci_test` over `/dev/ttyHS0` (`115200 8N1 CRTSCTS=1`) sent `EDL_PATCH_VER_REQ` (`01 00 fc 01 19`) and received the **21-byte QCA6390 ROM bootloader response**:
     `04 0e 12 01 00 fc 00 19 0c 10 00 00 00 2b 0d 00 02 00 02 0a 40`.
   - Exact Bluetooth firmware for QCA6390 v2.0 captured from Horizon OS `/vendor/bt_firmware/image/`: `htbtfw20.tlv` and `htnv20.bin`.
   - Note: `target-out/.config` still has `CONFIG_BT_HCIUART`, `CONFIG_BT_HCIUART_H4`, `CONFIG_BT_HCIUART_QCA`, `CONFIG_BT_RFCOMM`, `CONFIG_BT_BNEP`, `CONFIG_BT_HIDP` unset — enable these in the next kernel build (`#11`) for full native BlueZ over `/dev/ttyHS0`.

---

## Root-Cause Breakthroughs from Post-Jump #21 Horizon OS Forensics

### 1. 100% Root Cause of `PAS_INIT_IMAGE_CMD` (`-3145738` = `-0x30000A`) on Both Jump #19 and Jump #21
1. **Horizon OS `/reserved-memory` CMA Layout (`dmesg`)**:
   - On native Horizon OS, the `/reserved-memory` CMA pools sit at:
     - `linux,cma`: `0xfdc00000` (32 MiB — Horizon loads `a650_zap` relocatable image at `0xfdd00000..0xfdd01000`!)
     - `qseecom_region`: `0xfc800000` (20 MiB — actively protected in TZ/HYP by `vendor.qseecomd` for `attestation`, `ovrtz`, `widevine` TAs)
     - `adsp_region`: `0xfbc00000` (12 MiB), `cdsp_region`: `0xfb800000` (4 MiB), `sp_region`: `0xfa800000` (16 MiB), `cnss_wlan_region`: `0xf9400000` (20 MiB), `qseecom_ta_region`: `0xf8400000` (16 MiB), `sdsp_region`: `0xf7c00000` (8 MiB), `user_contig_region`: `0xf6c00000` (16 MiB), `secure_display_region`: `0xec800000` (164 MiB), `mailbox_region`: `0xffc00000` (0 MiB).
2. **Why `boot-130-j19.dtb` Triggered a 16 MiB `qseecom_region` Collision**:
   - `boot-130-j19.dtb` had `dtc -s` alphabetically sorted `/reserved-memory` child nodes (`user_contig_region` moved after `qseecom_region` / `linux,cma`, etc.), shifting `user_contig_region` to `0xfec00000` (16 MiB) and pushing **`linux,cma` down to `0xfcc00000..0xfebfffff`** — directly overlapping the upper 16 MiB of Horizon OS's still-protected **`qseecom_region` (`0xfc800000..0xfdbfffff`)**!
   - Therefore:
     - **Jump #19**: Stock `dma_alloc_attrs(..., DMA_ATTR_STRONGLY_ORDERED)` allocated `mdata_phys = 0x00000000fccbc000` directly inside Horizon OS's still-protected `qseecom_region` (`0xfc800000..0xfdbfffff`), so EL2 `protect_metadata` (`pil_map_memory(VMID_HLOS -> VMID_HYP)`) rejected it (`-3145738` = `-0x30000A`).
     - **Jump #21**: `qtee_shmbridge_allocate_shm()` allocated `mdata_phys = 0x00000001f8400000` inside `default_bridge` (SHM-bridge shared memory, not exclusive `VMID_HLOS`), which EL2 `protect_metadata` ALSO rejected with `-0x30000A`.
3. **Also Observed from `hyp_b.elf` + `/dev/block/by-name`**:
   - Active boot slot is `_b` (`androidboot.slot_suffix=_b`, `hyp_b` -> `/dev/sde23`, `hyp_xros_b` -> `/dev/sde40` with distinct md5sum, `tz_b` -> `/dev/sde22`).
   - EL2 intercepts `PAS_INIT_IMAGE_CMD` (`0x42000201`) in `pil_init_image_api_ns` -> `PIL_parseMetadata` -> `protect_metadata` -> `ACMapMemory(VMID_HLOS 3 -> VMID_HYP 4)`, negating `0x3000xx` errors via `cneg`.
4. **Clean Fix for Jump #22**:
   - Patch `private-captures/horizon-raw-fdt.dtb` in-place with **`fdtput` ONLY** (`/chosen/bootargs`, `/chosen/linux,initrd-start`, `/chosen/linux,initrd-end`) — **NEVER run `dtc -s`** — so `/reserved-memory` CMA addresses match Horizon OS cleanly.
   - Revert `qtee_shmbridge_allocate_shm()` in `drivers/soc/qcom/subsys-pil-tz.c` (`pil_init_image_trusted`) and use clean buddy pages (`__get_free_pages(GFP_KERNEL, get_order(PAGE_ALIGN(size)))` + `memcpy` + `__flush_dcache_area` + `virt_to_phys` + `free_pages`) + existing `PAS_SHUTDOWN_CMD` pre-clean retry.
   - Verify first on `pil_probe.ko` (`#10`) or directly in `Image` `#11`!

### 2. Exact Jump #21 Reset Cause from `/sys/fs/pstore/dmesg-ramoops-0`
- `[2021.013365] msm_watchdog 17c10000.qcom,wdt: Watchdog bark! Now = 2021.013334` (`Watchdog last pet at 2009.013032`, `ping pending mask from last pet 00`) -> `Kernel panic - not syncing: Panic triggered due to watchdog bark!`.
- `[2022.347670] SMP: failed to stop secondary CPUs 0-7`: CPUs 0–6 were idle in `lpm_cpuidle_enter`, while **CPU 7 was hard-locked with IRQs disabled** right after `/tmp/bt_hci_test` received the 21-byte EDL reply and continued into `tcdrain()` / `HCI_RESET` (shortly after `rfkill0` cycled `wl-reset-gpio(1120)` while `wlan.ko` was loaded). Fix in `bt_hci_test` / BT setup: avoid `tcdrain()` with `CRTSCTS` against ROM bootloader `HCI_RESET` and avoid toggling `wl-reset-gpio` while `wlan.ko` is active (`rfkill0` cycle before `insmod wlan.ko`, or test why CPU 7 locked during `rfkill0` + `msm_geni_serial`).

---

## Next Session Plan (After Sleep)
1. **Build in-place patched DTB (`boot-130-j22.dtb` via `fdtput` on `horizon-raw-fdt.dtb`) + Rebuild Kernel `Image` `#11`**:
   - Update `drivers/soc/qcom/subsys-pil-tz.c` (`pil_init_image_trusted` -> `__get_free_pages` + `__flush_dcache_area`, remove `qtee_shmbridge.h`).
   - Enable `CONFIG_BT_HCIUART`, `CONFIG_BT_HCIUART_H4`, `CONFIG_BT_HCIUART_QCA`, `CONFIG_BT_RFCOMM`, `CONFIG_BT_BNEP`, `CONFIG_BT_HIDP` in `target-out/.config`, run `olddefconfig Image` (`#11`), update `patches/target-kernel-stage5.diff`.
2. **Fire Clean Jump #22 (`Image` `#11` + `boot-130-j22.dtb`)**:
   - Check `/proc/iomem` and `dmesg | grep -E "linux,cma|qseecom_region"` to confirm CMA layout matches Horizon OS (`0xfdc00000` / `0xfc800000`).
   - Stage `/lib/firmware/a650_*`, `/lib/firmware/adsp.*` (`/dev/sde24` ro), and `/lib/firmware/qca/{htbtfw20.tlv,htnv20.bin}`.
   - Verify **`/tmp/gpu_test`** (Adreno 650 `/dev/kgsl-3d0` opens cleanly end-to-end!), **`boot_adsp` -> `/proc/asound/cards`** (Kona ADSP Audio), and **Bluetooth HCI** before `wlan.ko` / with native `hciattach`/`btattach`!
3. **Remaining Milestones After Step 1 Hardware Sign-Off**:
   - **Step 2**: Userspace Linux rootfs (Debian/Arch ARM64) + Wayland/DRM compositor + Mesa Turnip/Freedreno + PipeWire + BlueZ + iwd/NetworkManager.
   - **Step 3**: `syncboss` / SPI controllers / IMU / 6DoF / OpenXR (Monado).
   - **Step 4**: SteamOS / dual-boot packaging & clean initramfs launcher.
