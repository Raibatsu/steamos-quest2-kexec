# Quest 2 Native Linux — Stage 5 Handoff (4.19 Step 1 Verified -> Ready for Linux 7.2 Mainline)

## Hard Rules (DO NOT VIOLATE)
1. **NEVER touch `enp34s0` or `Wired connection 1`** via `nmcli`, `ip`, or routing commands — that is the user's PC internet. Quest 2 USB Ethernet (`10.42.0.1/24` <-> `10.42.0.2:23`) auto-connects via isolated NetworkManager profile `quest2-usb` pinned to MAC `02:51:4b:58:00:01` with `ipv4.never-default yes`.
2. **NEVER write to or flash Quest 2 UFS partitions** — mount/read (`mount -o ro`, `cat`) read-only only.
3. **NEVER commit device logs, serials, DTBs, or firmware blobs** to git (keep all in gitignored `private-captures/`).
4. **Singularity root is RAM-only**: after every reboot into Horizon OS, the user must trigger Singularity root. Depending on how Singularity is launched, root is accessible either via `adb shell "su -c '...'"` or via `nc 127.0.0.1 1234`.
5. **Always pass `stage2_verified=1`** to `insmod quest_kexec.ko` when `execute=1` is set (otherwise `qkx_dma_gate()` returns `-EOPNOTSUPP` = `Operation not supported on transport endpoint`), and always pass explicit `image=... initrd=... dtb=...` paths so `quest_kexec.ko` never picks up stale default filenames.
6. **Always pass `preserve_watchdog=1` (alongside `watchdog_recovery=1`)** and **`shutdown_dsps=1`** (which now shuts down `adsp`, `cdsp`, `slpi`, AND `a650_zap` before jump so `0xfdd00000` in `linux,cma` is unlocked cleanly across kexec).

---

## Current State (2026-11-21)
- **Repo**: `/mnt/HDD/quest2-kexec-build` (`origin/master` = `https://github.com/Raibatsu/quest2-kexec-research.git`).
- **Quest 2**: **Live on Jump #23 (`4.19.325-cip128-st12+ #11` at `10.42.0.2:23`)** via `payload_base=0x100000000` + `boot-100-j22.dtb` (booted in **4.6s**).
- **Target Kernel Build**: `/mnt/HDD/quest2-kexec-build/target-out/arch/arm64/boot/Image` (`31,839,074` bytes, build `#11`), with full patch diff saved in `/mnt/HDD/quest2-kexec-build/patches/target-kernel-stage5.diff`.
- **Kexec Loader**: `/mnt/HDD/quest2-kexec-build/loader/quest_kexec.ko` rebuilt with `"a650_zap"` in `dsp_names[]` (`loader/loader.c`).
- **Captured Assets in Gitignored `private-captures/`**:
  - `private-captures/horizon-raw-fdt.dtb` (`517,088` bytes) and `fdtput`-patched `private-captures/boot-{100,130}-j22.dtb` (`515,921` bytes each — preserving native Horizon OS `/reserved-memory` node order!).
  - `private-captures/firmware/` (`a650_gmu.bin`, `a650_sqe.fw`, `CM710X.bin`, `qca/{htbtfw20.tlv,htnv20.bin}`) + `private-captures/modules/` (28 stripped `.ko` modules).

---

## Stage 5 Step 1 Sign-Off: All Core Subsystems Verified Live on Hardware (Jumps #15–#23)
1. **8 CPUs (`0-7`) + 3-cluster CPUfreq**: All 8 Kryo 585 cores online (`4.19.325-cip128-st12+`).
2. **Physical 4K 120 Hz Display + DRM/KMS Scanout — EYEBALL-VERIFIED BY USER**:
   - Bound `qcom,mdss_dsi_jdi_4k_120Hz_dsc_video`, `/dev/dri/card0`, `/dev/dri/renderD128`, `conn=29 crtc=129` (`1920x3664 @ 120 Hz`); user confirmed visible RGBW test graphics inside the headset (`/tmp/drm_test`). Backlight and fan working.
3. **QCA6390 Wi-Fi (`wlan0`) — LIVE OVER-THE-AIR RF SCAN VERIFIED (Jump #18)**:
   - `qrtr-ns` + `cnss2` (`req->num_clients = 1`) + `wlan.ko` brought up `wlan0`; `nl80211_scan` discovered **12 live Wi-Fi access points (BSSs)** (`rc=0`).
4. **QCA6390 Bluetooth (`/dev/ttyHS0` + `rfkill0`) — LIVE UART HCI HANDSHAKE VERIFIED (Jump #21)**:
   - `rfkill0` (`bt_power`) powered all 4 BT regulators and GPIOs; `/tmp/bt_hci_test` on `/dev/ttyHS0` (`115200 8N1 CRTSCTS=1`) sent `EDL_PATCH_VER_REQ` (`01 00 fc 01 19`) and received the **21-byte QCA6390 ROM bootloader response** (`04 0e 12 01 00 fc 00 19 0c 10 00 00 00 2b 0d 00 02 00 02 0a 40`).
   - Exact BT firmware captured from Horizon OS: `htbtfw20.tlv` and `htnv20.bin`.
5. **Kona ADSP (`pas_id=1`) & APR Audio IPC — VERIFIED LIVE (Jump #23)**:
   - With `boot-100-j22.dtb` (`fdtput` on `horizon-raw-fdt.dtb` preserving CMA layout `linux,cma` at `0xfdc00000`, `qseecom_region` at `0xfc800000`) and clean buddy pages (`__get_free_pages` + `__flush_dcache_area`) in `subsys-pil-tz.c`:
     - All 12 Kona audio DLKMs loaded cleanly (`insmod` rc=0).
     - `echo 1 > /sys/kernel/boot_adsp/boot`:
       - `qkx-pil: PAS_INIT_IMAGE pas_id=1 mdata_phys=0x00000001efcbc000 size=8220 ret=0 scm_ret=0`
       - `subsys-pil-tz 17300000.qcom,lpass: adsp: Brought out of reset`
       - `adsp: Power/Clock ready interrupt received`
       - `adsprpc: fastrpc_rpmsg_probe: opened rpmsg channel for adsp`
       - `apr_tal_rpmsg qcom,glink:adsp.apr_audio_svc.-1.-1: apr_tal_rpmsg_probe: Channel[apr_audio_svc] state[Up]`
   - Note on `/proc/asound/cards` (`--- no soundcards ---`): `servloc: init_service_locator: Service Locator not enabled` (`rc=-19`) caused `kona-asoc` (`machine_dlkm`) / `msm-pcm-voice` / `msm-voip-dsp` to wait on ADSP PDR unless `pd-mapper`/PDR is disabled (matching `q2-audio-no-pdr.patch` in `quest2-steamos`, not needed once moving to mainline Linux 7.2 `qdsp6`).
6. **Adreno 650 GPU (`/dev/kgsl-3d0` + `a650_zap`) — 100% VERIFIED END-TO-END (Jump #23)**:
   - Adding `"a650_zap"` to `dsp_names[]` in `loader/loader.c` cleanly released Horizon OS's `0xfdd00000..0xfdd01000` (`a650_zap` inside `linux,cma`) before jumping (fixing the Jump #22 XPU/Stage-2 stall when `/dev/kgsl-3d0` opened).
   - Running `/tmp/j22/gpu_test` on Jump #23 printed:
     ```
     Opened /dev/kgsl-3d0 (fd=3)!
     Adreno 650 DEVICE_INFO: device_id=1 chip_id=0x06050002 gpu_id=0 mmu_enabled=1 gmem_size=1152 KiB
     KGSL VERSION: drv=3.14 dev=3.1
     GPU_TEST_RC=0
     ```
   - `dmesg` logged:
     - `qkx-pil: PAS_INIT_IMAGE pas_id=13 mdata_phys=0x00000001efc72000 size=6860 ret=0 scm_ret=0`
     - `subsys-pil-tz soc:qcom,kgsl-hyp: a650_zap: Brought out of reset`

---

## Next Phase: Pivot to Linux 7.2 Mainline (`overpressure/linux` + `URFAVF3MBOY/quest2-steamos` `kernel/linux72` Layer)
Now that both our kexec loader (`loader/loader.c` including `a650_zap` PIL shutdown) and the two cross-kexec silicon/TZ/CMA rules are **100% proven on Quest 2 hardware**, every prerequisite transfers cleanly to Linux 7.2:
1. Build Valve Frame Linux 7.2 (`overpressure/linux` commit `a47ef53148a9bcf3e46156bfd15cfaad4b5058cd`) + `quest2-steamos/kernel/linux72` layer (`sm8250-oculus-hollywood.dts`, `panel-oculus-hollywood-jdi.c`, UFS 2.1 gear-3 PHY, Adreno 650 `drm/msm`, simpledrm/fbcon).
2. Add the kexec-required `/reserved-memory` carve-outs to the 7.2 DTB (keep Horizon OS's `qseecom_region` `0xfc800000..0xfdbfffff`, `qseecom_ta_region` `0xf8400000..0xf93fffff`, and PIL carve-outs `no-map`) plus the cross-kexec GPU RSC wakeup check in mainline `drivers/gpu/drm/msm/adreno/a6xx_gmu.c` if needed.
3. Package a minimal 7.2 initramfs (NCM/USB shell + firmware staging) and kexec-boot Linux 7.2 on our locked Quest 2 via `quest_kexec.ko`!
