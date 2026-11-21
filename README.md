# `steamos-quest2-kexec` — Mainline Linux 7.2 & SteamOS on a Locked-Bootloader Meta Quest 2 via Kexec

> [!NOTE]
> **🤖 Project Origin & Disclosure:**
> This project is heavily scripted and engineered through an AI coding agent using spare AI credits I had lying around, paired with a locked-bootloader Meta Quest 2 (`hollywood`) that was otherwise collecting dust on a shelf due to data/privacy concerns.
>
> **Zero bootloader unlock required. Zero UFS system/boot partition flashing required.** Everything boots purely from RAM via a loadable kernel module (`quest_kexec.ko`) injected from a temporary root shell on Horizon OS, and can self-`kexec` from **Linux 7.2 → Linux 7.2 in ~6.4 seconds** without rebooting back into Horizon OS.

---

## 📊 Hardware Bring-Up Progress (Live-Verified on Retail Locked Quest 2)

| Subsystem | Kernel 4.19 (Stage 5) | Mainline Linux 7.2 (`7.2.0-hollywood+`) | Details / Driver |
| :--- | :---: | :---: | :--- |
| **Kexec EL1 Jump (`quest_kexec.ko`)** | ✅ Verified | ✅ Verified | `Horizon OS 4.19` → `Linux 7.2` in ~5.2 s (`a650_zap` + `adsp`/`cdsp`/`slpi` PIL teardown, SMMU/DWC3/UFSHCD quiesce) |
| **Self-Kexec (`Linux 7.2` → `Linux 7.2`)** | N/A | ✅ **6.4 s** | Native `/bin/q2kexec` (`kexec_file_load` & `-D <dtb>` `SYS_kexec_load` with 40-byte ARM64 trampoline + `dwc3`/`qcom_aoss` `.shutdown`) |
| **8× Kryo 585 SMP + CPUfreq** | ✅ Verified | ✅ Verified | All 8 cores (`CPU 0–7`) online via PSCI + `qcom-cpufreq-hw` |
| **USB-C ConfigFS Gadget (NCM + ACM)** | ✅ Verified | ✅ Verified | `a600000.usb` (`dwc3`), `10.42.0.2:23` telnet + `/dev/ttyGS0` root shell + `udhcpd` |
| **Internal UFS 2.1 Storage (6 LUNs)** | ✅ Verified | ✅ Verified | `1d84000.ufshc` (`sd_mod` `/dev/sda`..`/dev/sdf`, Gear-3 rate-B PHY init) |
| **Adreno 650 GPU + GMU + Command Ring** | ✅ Verified (`kgsl-3d0`) | ✅ Verified (`drm/msm`) | `/dev/dri/renderD128` (`gpu_id=650`, `587 MHz`, `a650_sqe.fw` + `a650_gmu.bin` + signed `a650_zap.mdt`, `PKT7(CP_MEM_WRITE)` ring-0 fence pass + cross-kexec PDC/RSC sequence clear) |
| **Dual-DSI 1920×3664 JDI VR Display** | ✅ Verified (`msm_drm`) | ✅ Verified (`dpu1` + `dsi0/1`) | Cold rail bring-up (`qcom-pm8150l-lcdb` `±5.7 V`, `vreg_l14a_1p8` `1.8 V`, `vreg_l11c_3p3` `3.3 V` BLU MCU, `panel-oculus-hollywood-jdi`), stereo side-by-side (`1832×1920` per eye @ `rot=270`) visual confirmation |
| **QCA6390 Wi-Fi & Bluetooth** | ✅ Verified (`wlan0` + `ttyHS0`) | 🔄 Next (Stage 2) | PCIe + UART HCI (`htbtfw20.tlv` / `htnv20.bin`) verified in Stage 5; plumbing into 7.2 Stage 2 next |
| **Kona ADSP (`pas_id=1`) & Audio** | ✅ Verified (`apr_audio_svc`) | 🔄 Next (Stage 2) | PIL auth (`PAS_INIT_IMAGE`) & GLINK up; mainline `qdsp6` in progress |
| **Stage 2 SteamOS Rootfs (`switch_root`)** | 🔄 Ready | 🔄 In Progress | Btrfs + OverlayFS + NBD + USB Mass Storage compiled in; Stage 2 boot next |

---

## 🧠 Why Kexec on Quest 2 Was Hard (and How We Solved It)

Booting a fresh kernel via `kexec` from Meta's running Horizon OS (Android 12L / Linux 4.19.157) without going through `abl`/`xbl` cold reset hits several hardware traps on Qualcomm SM8250 (`kona`):

1. **Hyp / QSEE / PIL Locked CMA (`0xfdd00000` XPU Violation):**
   - Horizon OS loads the signed GPU zap shader (`a650_zap`, `pas_id=13`) into `linux,cma` (`0xfdc00000..0xffffffff`) and locks those physical pages in the hypervisor Stage-2 page tables.
   - If the kexec loader does not call `subsystem_put()` on `a650_zap` (alongside `adsp`, `cdsp`, and `slpi`) before jumping, the target kernel's buddy allocator hands out `0xfdd00000` and triggers an instant **Hyp XPU violation / watchdog reset** the first time the target kernel touches that page.
   - **Fix:** [`loader/loader.c`](loader/loader.c) explicitly shuts down `adsp`, `cdsp`, `slpi`, and `a650_zap` prior to DMA teardown, and the target DTB marks Horizon OS's active `qseecom_region` (`0xfc800000`, `20 MiB`) and `qseecom_ta_region` (`0xf8400000`, `16 MiB`) as `no-map`.

2. **Adreno 650 GMU `GPU_PDC_SEQ_START_ADDR` Stale Across Kexec:**
   - When Horizon OS's KGSL driver runs before kexec, it leaves the GPU RSC sequencer's `PDC_GPU_SEQ_START_ADDR` non-zero.
   - Mainline Linux 7.2's `a6xx_gmu_rpmh_init()` in `drivers/gpu/drm/msm/adreno/a6xx_gmu.c` checks `if (!pdc_address_offset) goto err_pdc;`—WAIT, it actually skips programming the PDC sequence if the register is already non-zero, leaving GPU RSC microcode uninitialized so `RSCC_SEQ_BUSY_DRV0` times out (`-ETIMEDOUT`).
   - **Fix:** Clear `GPU_PDC_SEQ_START_ADDR` (`0`) before calling `a6xx_gmu_rpmh_init()` in [`patches/linux72-hollywood-kexec.diff`](patches/linux72-hollywood-kexec.diff).

3. **Cold Display & Backlight Rails After Horizon OS Screen-Off:**
   - When Horizon OS turns the screen off before kexec, it powers down three critical panel supplies:
     - `vreg_l14a_1p8` (`pm8150` LDO14 `1.8 V` VDDIO)
     - `pm8150l_lcdb` (`pm8150l` SPMI USID 5 `lcdb@ec00` `±5.7 V` LDO/NCP + `5.85 V` boost)
     - `vreg_l11c_3p3` (`pm8150l` LDO11 `3.3 V` Backlight Unit MCU rail)
   - **Fix:** Added a dedicated mainline PM8150L LCDB regulator driver ([`linux72/drivers/qcom-pm8150l-lcdb.c`](linux72/drivers/qcom-pm8150l-lcdb.c)) and wired the full power-up sequence + JDI `0xB9` DCS backlight brightness control into [`linux72/drivers/panel-oculus-hollywood-jdi.c`](linux72/drivers/panel-oculus-hollywood-jdi.c) and [`linux72/dts/sm8250-oculus-hollywood-kexec.dts`](linux72/dts/sm8250-oculus-hollywood-kexec.dts).

4. **Fast 6.4s Linux 7.2 → Linux 7.2 Self-Kexec (`dwc3` SMMU Fault & AOSS QMP Ack):**
   - Mainline `platform/dwc3` (`a600000.usb`, attached to `15000000.iommu`) and `platform/dwc3-qcom-legacy` (`a6f8800.usb`) had **no `.shutdown` callbacks**, leaving USB gadget DMA rings active while `arm_smmu_device_shutdown()` disabled SMMU translation (`CLIENTPD=1`).
   - **Fix:** Added `dwc3_shutdown()` and deadlock-free `dwc3_qcom_shutdown()` + AOSS QMP link-down ack wait in [`patches/linux72-hollywood-kexec.diff`](patches/linux72-hollywood-kexec.diff), plus `/bin/q2kexec` ([`linux72/tools/q2kexec.c`](linux72/tools/q2kexec.c)) supporting both `kexec_file_load` and `-D <dtb>` `SYS_kexec_load` with a 40-byte ARM64 DTB-override trampoline.

---

## 📁 Repository Layout

```text
steamos-quest2-kexec/
├── loader/                                # Horizon OS 4.19 kexec kernel module (quest_kexec.ko)
│   ├── loader.c                           # Payload loader, PIL/DSP teardown (adsp/cdsp/slpi/a650_zap), RAM verifier
│   ├── shutdown.c / shutdown_io.c         # Device tree & hardware DMA/SMMU/UFS/USB quiesce sequence
│   ├── transition.S                       # EL1 identity-mapped MMU-off cache-clean kexec trampoline
│   └── Makefile
├── backends/                              # Modular subsystem preflight & shutdown research backends
├── linux72/                               # Mainline Linux 7.2 (overpressure/linux + quest2-steamos layer)
│   ├── dts/
│   │   ├── sm8250-oculus-hollywood.dts            # Base Quest 2 (Hollywood) mainline device tree
│   │   ├── sm8250-oculus-hollywood-kexec.dts      # Kexec overlay (PM8150L LCDB, regulators, QSEE no-map)
│   │   └── sm8250-oculus-hollywood-dtbo-sink.dtsi # Bootloader DTBO symbol sink
│   ├── drivers/
│   │   ├── panel-oculus-hollywood-jdi.c           # JDI 1920x3664 dual-DSI VR panel + DCS backlight driver
│   │   └── qcom-pm8150l-lcdb.c                    # Qualcomm PM8150L ±5.7V LCDB bias regulator driver
│   ├── tools/
│   │   ├── q2kexec.c                              # Linux 7.2 -> Linux 7.2 self-kexec tool (with -D <dtb> trampoline)
│   │   ├── stereo_test.c                          # Side-by-side 1832x1920/eye @ rot=270 VR stereo test pattern
│   │   ├── msm_gpu_test.c                         # Adreno 650 drm/msm ring-0 CP_MEM_WRITE submission test
│   │   └── verify_and_pack_dtb.py                 # Packs kexec DTB for 0x100000000 / 0x130000000 / 0x90600000
│   ├── hollywood.config                   # Base hollywood config fragment
│   └── hollywood-kexec.config             # Kexec + Btrfs + OverlayFS + F2FS + NBD + LCDB config fragment
├── patches/
│   ├── linux72-hollywood-kexec.diff       # Full patch against overpressure/linux (a47ef53148a9)
│   └── target-kernel-stage5.diff          # Stage 5 downstream 4.19 target kernel patch
├── DEVLOG.md                              # Detailed engineering log of all kexec jumps & root-cause analyses
└── HANDOFF_STAGE5.md                      # Stage 5 (4.19) subsystem verification sign-off notes
```

---

## 🚀 How to Build & Test on Your Own Locked Quest 2

### Prerequisites
1. **A Meta Quest 2 (`hollywood`)** with a temporary root method on Horizon OS (e.g., Singularity / exploit root giving a root shell on `4.19.157-perf+`). **Do NOT unlock or flash your bootloader.**
2. **Linux x86_64 host PC** with `aarch64-linux-gnu-gcc`, `clang` / `lld` (AOSP Clang r383902b for the 4.19 `quest_kexec.ko` module), `dtc`, `python3`, and `adb`.
3. **Host USB Network (`10.42.0.1/24`)**:
   Configure your host PC so that when the Quest 2 enumerates its ConfigFS USB NCM Ethernet gadget (`10.42.0.2`), your host interface gets `10.42.0.1/24` (the Stage 1 initramfs also runs `udhcpd` offering `10.42.0.1`).

---

### Step 1: Build Mainline Linux 7.2 (`7.2.0-hollywood+`)

Clone the Valve Frame / SM8250 mainline kernel tree ([`overpressure/linux`](https://gitlab.steamos.cloud/overpressure/linux) at commit `a47ef53148a9bcf3e46156bfd15cfaad4b5058cd`) and apply our Hollywood kexec layer:

```bash
# 1. Apply the Linux 7.2 patch & copy new files into the kernel tree
cd /path/to/overpressure-linux
git checkout a47ef53148a9bcf3e46156bfd15cfaad4b5058cd
git apply /path/to/steamos-quest2-kexec/patches/linux72-hollywood-kexec.diff

cp /path/to/steamos-quest2-kexec/linux72/dts/* arch/arm64/boot/dts/qcom/
cp /path/to/steamos-quest2-kexec/linux72/drivers/panel-oculus-hollywood-jdi.c drivers/gpu/drm/panel/
cp /path/to/steamos-quest2-kexec/linux72/drivers/qcom-pm8150l-lcdb.c drivers/regulator/
cp /path/to/steamos-quest2-kexec/linux72/hollywood*.config arch/arm64/configs/

# 2. Configure and build Image + DTBs
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig hollywood.config hollywood-kexec.config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image qcom/sm8250-oculus-hollywood-kexec.dtb
```

---

### Step 2: Build the Userspace Tools (`q2kexec`, `stereo_test`, `msm_gpu_test`)

```bash
aarch64-linux-gnu-gcc -O2 -static -o q2kexec linux72/tools/q2kexec.c
aarch64-linux-gnu-gcc -O2 -static -o stereo_test linux72/tools/stereo_test.c
aarch64-linux-gnu-gcc -O2 -static -o msm_gpu_test linux72/tools/msm_gpu_test.c
```

Embed `q2kexec` into `/bin/q2kexec` inside your Stage 1 BusyBox initramfs (`initramfs-stage1.cpio.gz`), then run `linux72/tools/verify_and_pack_dtb.py` to stamp the exact `linux,initrd-start` and `linux,initrd-end` offsets into:
- `boot-100-linux72.dtb` (`payload_base=0x100000000`)
- `boot-130-linux72.dtb` (`payload_base=0x130000000`)
- `boot-906-linux72.dtb` (`0x90600000`, for `q2kexec -D` self-kexec inside Linux 7.2)

---

### Step 3: Jump from Horizon OS into Mainline Linux 7.2

Push `quest_kexec.ko`, `Image`, `initramfs-stage1.cpio.gz`, and `boot-100-linux72.dtb` (or `boot-130-linux72.dtb`) to `/data/local/tmp/` on the Quest 2, then from your Horizon OS root shell:

```bash
# Compact memory first so alloc_contig_range() succeeds cleanly even after long Horizon OS uptime:
echo 3 > /proc/sys/vm/drop_caches
echo 1 > /proc/sys/vm/compact_memory

# Validate contiguous 56 MiB window at 0x100000000 (or 0x130000000):
insmod /data/local/tmp/quest_kexec.ko validate_ram=1 ram_base=0x100000000 ram_bytes=0x3800000
rmmod quest_kexec

# Execute the kexec jump into Linux 7.2:
insmod /data/local/tmp/quest_kexec.ko \
  execute=1 stage2_verified=1 \
  payload_base=0x100000000 \
  image=/data/local/tmp/Image \
  initrd=/data/local/tmp/initramfs-stage1.cpio.gz \
  dtb=/data/local/tmp/boot-100-linux72.dtb \
  quiesce_Mode=2 shutdown_dsps=1 \
  watchdog_recovery=1 preserve_watchdog=1
```

Within **~5 seconds**, USB re-enumerates as `Quest 2 stage 1` (`10.42.0.2`). Connect via telnet:

```bash
telnet 10.42.0.2 23
```

---

### Step 4: Verify Display Stereo Side-by-Side, Adreno 650 GPU, and 6.4s Self-Kexec

Once inside the Linux 7.2 shell on `10.42.0.2:23`:

```bash
# 1. Render the upright side-by-side stereo 3D test scene on the headset lenses:
/tmp/stereo_test
# -> STEREO_SBS_OK (1920x3664 stride=7680)

# 2. Test Adreno 650 GPU ring-0 command submission:
/tmp/msm_gpu_test
# -> Readback: dst[0]=0xcafebabe dst[1]=0x51554553 -> GPU_SUBMIT_PASS

# 3. Reboot directly into a new Linux 7.2 kernel / initramfs / DTB in 6.4 seconds
#    (NO Horizon OS reboot or re-rooting required!):
/bin/q2kexec -D /tmp/boot-906-linux72.dtb /tmp/Image /tmp/initramfs.cpio.gz
```

---

## 🙏 Credits & Acknowledgments

- **[`URFAVF3MBOY/quest2-steamos`](https://github.com/URFAVF3MBOY/quest2-steamos)** — For the incredible Quest 2 SteamOS port, `panel-oculus-hollywood-jdi.c`, `sm8250-oculus-hollywood.dts`, and Stage 2 SteamOS infrastructure.
- **[`overpressure/linux`](https://gitlab.steamos.cloud/overpressure/linux)** — Mainline Linux 7.2 SM8250 kernel tree.
