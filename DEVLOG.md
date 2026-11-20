# Quest 2 kexec — independent development log

Device: Meta Quest 2 (hollywood), firmware `UP1A.231005.007.A1`, build
`52242990024200150`, kernel `4.19.325-cip128-st12-g49638c7a8637`, Magisk root,
bootloader locked (`ro.boot.verifiedbootstate=green`, `ro.boot.flash.locked=1`).

Based on `WitherBlitz/Quest-2-Steam-OS-Dual-Boot` (fork of
`machachu56/quest-kexec-q2`), whose documented development identity matches
this device exactly.

## What we ruled out first

- **Bootloader unlock**: CVE-2021-1931 only works on firmware ≤ May 2021; this
  device is far newer. No public exploit found.
- **Qualcomm EDL route**: entering EDL works (`adb reboot edl` →
  `05c6:9008`). Sahara handshake succeeds and reaches image transfer, but two
  generic SM8250 loaders (BlackShark-signed, from the `hoplik/Firehose-Finder`
  catalog) both failed partway through transfer — one via repeated I/O errors
  + USB reset, one via a silent stop right after the signature-bearing chunk
  was sent. No Meta-signed Firehose loader exists publicly. Treated as a dead
  end without one.
- **OpenABL direct-write route**: writes `abl_a`/`abl_b` via root `dd`,
  bypassing fastboot's lock check — but Quest 2's secure-boot chain
  (`verifiedbootstate=green`) almost certainly rejects an unsigned ABL at the
  XBL level, with no known Meta-signed EDL loader to recover via if it does.
  Never attempted on this (only) device.

## What we built ourselves

1. **Android kernel source**: `facebookincubator/oculus-linux-kernel` at
   `6bde5fc06b252c97761bc0963dd13c1abcf377ab` — confirmed by its own commit
   message ("Oculus Quest 2 kernel project build: 5224299.2420.150") to be the
   exact source for our firmware build.
2. Patched `scripts/extract-cert.c` to drop OpenSSL ENGINE/pkcs11 support
   (unneeded; OpenSSL 3/4.x on this host removed the deprecated ENGINE API).
3. Built `vmlinux` + `modules_prepare` only (not full `modules` — avoids the
   entire qcacld-3.0 Wi-Fi staging driver, which doesn't compile cleanly
   under modern clang's strictness defaults). Derived `Module.symvers` via
   `modpost` directly from `vmlinux`.
   - **11,137 exports** — exact match to the project's own documented
     reference count. Strong independent confirmation the build is correct.
4. Built `quest_kexec.ko` / `marker_read.ko` out-of-tree against that
   `Module.symvers`, **without** `QKX_EXPERIMENTAL=1` — meaning
   `QKX_ENABLE_EXPERIMENTAL_JUMP` is undefined and the real jump path
   (`execute`, CPU/GPU rehearsals, watchdog/core-hang/secure-watchdog
   control, QMP/Syncboss/RPMH hooks) is **compile-time absent**. Verified
   directly in `loader.c`: all of those parameters are hard-rejected with
   `-EOPNOTSUPP` before any symbol resolution or hardware access, in a build
   like ours.
   - `vermagic` matched the running kernel exactly on the first build.

## On-hardware results (all via `validate_ram=1` — claim-and-release only,
no payload, no shutdown, same mechanism the project's own prior tests used)

Ran their read-only `quest2_preflight.py` capture (identity matched exactly)
and `quest2_handoff_plan.py` planner against it — produced the same 8
candidate 52 MiB windows above 4 GiB their own planner found.

| Candidate | Base | 52 MiB result | 36 MiB result |
|---|---|---|---|
| 1 | `0x100000000` | (not retried; known busy ~44 MiB in) | **owned=1** ✅ |
| 2 | `0x103400000` | refused, PFN `0x105c00` unsupported pageblock | **owned=1** ✅ |
| 3 | `0x106800000` | refused, PFN `0x107400` | refused |
| 4 | `0x109c00000` | refused, PFN `0x10b800` | refused |
| 5 | `0x10d000000` | refused, PFN `0x10f800` | **owned=1** ✅ |
| 6 | `0x110400000` | refused, PFN `0x112800` | refused (unmovable block at `0x110800`) |
| 7 | `0x113800000` | refused, PFN `0x113800` | refused |
| 8 | `0x116c00000` | refused, PFN `0x118800` | refused |

Three independently verified safe 36 MiB claim/release windows. Device fully
healthy after every attempt — no panic/BUG/WARNING/oops/fault anywhere in
dmesg, `sys.boot_completed` stayed `1` throughout, taint transitioned cleanly
`1028 → 5124` (expected out-of-tree-module flag) and back on unload.

This closes out the project's own open item: *"the other seven candidates
remain untested."*

## Target Linux payload (built, not yet staged)

- Copied kernel source to a separate git-tracked tree
  (`kernel-target-src`), applied both `kernel/patches/quest2-startup/*`
  patches via `git apply` — base and post-patch hashes verified byte-for-byte
  against the project's own reviewed values before and after.
- Built target `Image` (31.8 MB) from their `target.config`
  (hash-verified against their release asset).
- Built static ARM64 BusyBox (`busybox.net`, pinned 1.36.1) + initramfs
  (1.14 MB) via the project's own `initramfs/busybox.sh` + `build.py`.
- Flattened our own preflight capture's live device tree with `dtc -I fs -O
  dtb` (built as a side effect of the kernel build).
- Ran `tools/quest2_target_dtb.py` with a corrected provenance file (their
  released `target-build-provenance.json`'s file hashes are for *their*
  build; ours differ since it's a different compiler/environment, matching
  their own stated "not a cross-machine rebuild claim" caveat — rebuilt the
  provenance's `files` section with our actual hashes, kept every other
  pinned field identical since the patches/source hashes matched exactly).
  - Result: **`PRIVATE_DTB_PREPARED; HANDOFF_BLOCKED; 0 structural/profile
    findings`**.
  - Recommended base: `0x100000000` — same window as our already-verified
    candidate 1.

## Next: `payload_base` staging test

Traced `loader.c` function-by-function before running anything:
`qkx_claim_destination()` (the only function that touches the real
destination physical range) is only called when `execute` is set — which is
impossible in our build. `qkx_stage()` only ever reads file bytes into
freshly, ordinarily allocated scratch pages (`qkx_page()` → `alloc_page()`),
explicitly skipping any page that happens to physically fall inside the
destination window. `qkx_identity_map()` likewise only builds a private page
table inside scratch pages — never installs it, never touches the live
kernel's page tables. `qkx_release()`, called on any init error and
unconditionally on `rmmod`, frees every page the module ever allocated.

Conclusion: a `payload_base`-only run (no `execute`) never touches, claims,
or writes to the real destination RAM at all. Smaller real-world footprint
than the `validate_ram` tests already run three times successfully.

## Staging test result (2026-11-20)

Pushed `Image`/`initramfs.gz`/`boot.dtb` to `/data/local/tmp/`, hashes
verified matching on-device before load.

```
insmod quest_kexec.ko payload_base=0x100000000 image=... initrd=... dtb=...
→ INSMOD_EXIT=0
quest_kexec: segment dest=100080000 file=31838983 memory=33656832 crc32=c0c4abac
quest_kexec: segment dest=102099000 file=1141452 memory=1142784 crc32=cdf4e29c
quest_kexec: segment dest=1021b0000 file=506829 memory=507904 crc32=f757ab5f
quest_kexec: staged 8620 copy pages, 8665 allocations; entry=100080000 dtb=1021b0000; execute=0
```

All three CRC32s independently verified against our local files (zlib.crc32)
— exact match, confirming bit-correct staging.

`rmmod` → `quest_kexec: unloaded; staging memory released`. Module gone from
`lsmod`. No panic/BUG/WARNING/oops/fault anywhere in dmesg. `boot_completed=1`
throughout, uptime continuous (no reboot), taint `5124` (expected, clears on
reboot).

**Status: real kernel+initramfs+DTB payload staged and verified in scratch
memory on real hardware, with the actual destination RAM window never
touched. This is the furthest point reachable without `QKX_EXPERIMENTAL=1`
— the real jump path remains compile-time absent in every module we've
built.**

## QKX_EXPERIMENTAL=1 test (2026-11-20)

Rebuilt with `QKX_EXPERIMENTAL=1`. Confirmed via `strings`/`nm` that the old
compile-time refusal string is gone and `qkx_execute` is now a real symbol —
the jump path is genuinely compiled in this time, not just claimed.

Read `qkx_dma_gate()` in full before testing:
```c
static int qkx_dma_gate(void)
{
    const char *model;
    if (of_property_read_string(of_root, "model", &model))
        return -ENODEV;
    if (!strncmp(model, "Hollywood", 9)) {
        pr_err("quest_kexec: Hollywood DMA shutdown/completion is unvalidated; refusing hardware transition\n");
        return -EOPNOTSUPP;
    }
    return 0;
}
```
Pure DT string read, zero side effects, called before `qkx_resolve()` and any
hardware access, independent of the compile flag.

Tested `insmod quest_kexec.ko execute=1` on-device:
```
insmod: failed to load ...: Operation not supported on transport endpoint
dmesg: quest_kexec: Hollywood DMA shutdown/completion is unvalidated; refusing hardware transition
```
Module never loaded (`lsmod` empty). No panic/BUG/WARNING/fault. Device
healthy, uptime continuous.

**Confirmed, not just documented: the board gate refuses the real jump path
on this device even with the experimental flag compiled in.** Going further
would require editing/removing this specific check — a deliberate act against
an explicit, reasoned safety boundary, not a flag flip. Not attempted; not
planned without a real solution to the DMA/camera/SMMU teardown problem this
gate exists to enforce.

## Option 2: moving toward overpressure / Valve's 7.2 kernel (2026-11-20, continued)

Not waiting on URFAVF3MBOY's answer to issue #7/#34 — starting this
ourselves, following the `overpressure/phoenix` (PICO 4) pattern as the
worked example, since PICO 4 and Quest 2 share the SM8250/XR2 SoC.

### Layering model (from `overpressure/phoenix/docs/kernel.md`)

`overpressure/linux` builds Valve's `linux-valve` 7.2.0 in layers, each a
branch built on the one below, merged by PR, never rebased:
- `valve/7.2` — mirror of Valve's own tree (weekly, via evlaV)
- `common/7.2` — fixes any headset on Valve's tree needs (DDR info, root
  identity, BCL, Bluetooth firmware wait, DSP handover, FastRPC, PSCI reset,
  CI)
- `sm8250/7.2` — SM8250-generic: Venus (video codec), CAMSS, A650 GPU
  preemption, DPU 6.0 refresh changes — **already shared infrastructure, no
  Quest-2-specific work needed here**
- `phoenix/7.2` — PICO 4 only: panel, battery, tracking/RGB camera drivers,
  audio, device tree, `pico4.config`

A `hollywood/7.2` layer on top of `sm8250/7.2` is exactly where
`overpressure/overpressure#34` already suggests Quest 2 would go.

**Major overlap with PICO 4, already solved by someone:**
- **QCA6390 Wi-Fi/Bluetooth** — identical chip to PICO 4. Phoenix uses
  `ath11k` + `hci_qca`, PICO's firmware/NV data from their `bluetooth`
  partition. `quest2-steamos` already has working QCA6390 Bluetooth patches
  on the *vendor* kernel (`patches/q2-bt-hci-qca-ldisc.patch`) — real
  reference for NVM/baud-rate/sleep-bit quirks that should translate.

### Key find: Meta published the real Quest 2 device tree source

`facebookincubator/oculus-linux-kernel` (which we already have in full, see
above) contains the actual, maintained, per-subsystem Linux DTS for
"hollywood" at `arch/arm64/boot/dts/oculus/hollywood/`:
`hollywood.dtsi` (main board), `hollywood-panel.dtsi` + 15 panel-timing
variants (sharp/jdi/jdi-nvt/boe × 72/80/90/120 Hz), `hollywood-qca6390.dtsi`,
`hollywood-syncboss.dtsi`, `hollywood-camera.dtsi`, `hollywood-audio.dtsi`,
`hollywood-ufs.dtsi`, `hollywood-thermal.dtsi`, `hollywood-fan.dtsi`,
`hollywood-adc.dtsi`, battery profile dtsi files, board-revision overlays
(evt1/evt2/evt/dvt/poc2/p1.1/pvt-b); base platform layer
`kona-v2-oculus.dts`/`kona-v2.1-oculus.dts`.

This is **categorically better than what PICO 4's port had** — phoenix's own
docs say PICO's vendor kernel source was never published, forcing
reverse-engineering by hardware testing (their own doc: *"1.904 V stalls CPU
0 (hard lockup); the tree uses 1.896 V"* — found by trial and error, not
documentation). We have the real, intentional, documented answer already.

Spot-checked `hollywood-qca6390.dtsi`: exact GPIO wiring for Bluetooth
(`BT_EN`=tlmm21, `SW_CTRL`=tlmm124, `WL_EN`=tlmm20) and regulator voltage
rails (S6A/S5A/S8C at specific mV). `hollywood.dtsi` also reveals the fuel
gauge is `ti,bq27z561` on `qupv3_se4_i2c` — **TI's BQ27xxx family has a
well-supported mainline driver** (`drivers/power/supply/bq27xxx_battery.c`),
unlike PICO 4 which needed a brand-new PM8150B driver written from scratch.
Battery monitoring may need device-tree wiring only, not a new driver.

`hollywood-syncboss.dtsi` confirms SPI-based tracking MCU with explicit GPIO
wiring (wakeup=GPIO4, reset=GPIO5, float_swd=GPIO52/53) — corroborates
`quest2-steamos`'s own kexec notes (`"Syncboss is spi1.0"`) and our own live
device-tree capture from tonight, three independent sources agreeing.

Secondary source checked: `meowybnuuy/android_device_oculus_hollywood`
(AOSP/TWRP device config, stale at Android 12.1, not a kernel DTS). Useful
only as corroboration: A/B partition scheme, boot image header v2,
`androidboot.hardware=hollywood`, kernel cmdline fragments — all consistent
with what we already know. Not a device-tree source despite the name.

### Honest scope assessment

Device-tree/board-wiring risk is now substantially lower than a from-scratch
port — it's translation (downstream Qualcomm/Oculus DTS idioms →
mainline/mainline-adjacent idioms Valve's tree uses) against real, documented
source, not reverse-engineering. The dominant remaining effort is still
driver work matching what phoenix/7.2's own commit list needed for PICO:
a panel driver, a SyncBoss (tracking MCU) driver, audio bring-up — genuine
multi-week kernel engineering, but with meaningfully less unknown risk than
initially scoped, and with `quest2-steamos`'s existing vendor-kernel work as
a second cross-check for several of these subsystems.

### Repo work started

- Cloned `overpressure/overpressure`, `overpressure/phoenix`,
  `overpressure/headset-template` for reference.
- Fetched `overpressure/linux` at `sm8250/7.2` (shallow, single branch).
- Not yet scaffolded `overpressure/hollywood` from the template — next step.

## DMA/camera/SMMU teardown research (kexec-safety problem)

Resumed the kexec angle specifically to investigate what it would take to
safely quiesce camera/ADSP/SMMU DMA before a real jump — the thing
`qkx_dma_gate()` currently refuses to allow for Hollywood, precisely because
nobody has solved it.

**`shutdown.c`/`shutdown.h` in the WitherBlitz module is a generic
transactional orchestrator, not a solution.** It defines a clean
forward/rollback contract (deadlines, receipts, per-subsystem "backends"
with inspect/begin/poll/rollback hooks) across ten subsystems (camera, DSP,
GPU, SyncBoss, USB, UFS, other-DMA, SMMU, secure, AOP). But grepping the
whole repo, the only place any `qkx_shutdown_backend` gets populated is
`tools/tests/shutdown_harness.c` — a **host-side unit test with mock
backends**. No real camera/ADSP/SMMU-touching backend exists anywhere.
The hard part (the actual device-specific shutdown+verification code) is a
complete blank slate, even in this most advanced known attempt.

Checked three real subsystems in Meta's own kernel source for what
shutdown/completion primitives actually exist to build real backends on:

- **Camera (`cam_req_mgr_dev.c`)**: `cam_req_mgr_close()` is a plain v4l2
  file-release callback, gated by a simple (non-atomic) `open_cnt` — runs
  automatically when the last userspace fd closes or its owning process
  dies. This means the "how do we stop it" half is already solved by
  Android's normal process/fd lifecycle (every reboot does this). But no
  independent "zero DMA in flight" signal was found: `cam_ife_hw_mgr.c`'s
  only similar-looking flag (`overflow_pending`) is a hardware-overflow
  *error* indicator, not an idle/done flag. Trusting `close()` alone means
  trusting the driver did its job completely, with no second check.
- **SMMU (`arm-smmu-debug.c`)**: has real TBU/TCU hardware test-bus register
  access (`arm_smmu_debug_tbu_testbus_select/output`), which could in
  principle reveal outstanding-transaction state — but interpreting the
  test-bus output requires vendor-internal bit-encoding documentation this
  tree doesn't include. Not usable without that, short of risky
  trial-and-error on the only unit we have.
- **ADSP/CDSP/SLPI (`subsystem_restart.c`)**: the real win. `subsys_stop()`
  does `notify_each_subsys_device(..., SUBSYS_BEFORE_SHUTDOWN, ...)`, then
  `sysmon_send_shutdown()` — a **synchronous, two-way IPC handshake with the
  remote firmware itself**, giving the DSP's own software a chance to idle
  its own DMA before the AP does anything hardware-level — then the PIL
  `shutdown()` callback (the actual reset-assert), then only *after* all of
  that, `SUBSYS_AFTER_SHUTDOWN` fires. This is genuine, checkable completion
  evidence (a real return code from a real firmware round-trip), not a
  hopeful one-way call. Qualcomm's existing `subsystem_restart` API
  (`subsystem_put()`) already does this correctly — no new mechanism needs
  inventing here, just correct wiring plus actually checking
  `sysmon_shutdown_ret` instead of ignoring it.

### Honest standing

Of the three subsystems checked, only ADSP/CDSP/SLPI has a trustworthy,
already-built completion signal. Camera's existing close path is usable but
unverified-complete. SMMU's debug path exists but is unusable without
documentation we don't have. GPU, UFS, USB, SyncBoss, secure/TZ, and AOP are
still completely unexamined. This is real, incremental de-risking of one
subsystem out of ten — not a solved problem. Building real backends for the
remaining subsystems, and deciding what to do about camera/SMMU's weaker
evidence, is still open, multi-session kernel research.

### GPU and UFS checked — both have strong, direct hardware evidence

- **GPU (Adreno/KGSL, `adreno.c`)**: `adreno_isidle()` reads the GPU's own
  ringbuffer read/write pointer *hardware registers* directly (`rptr`/`wptr`,
  with an `smp_mb()` barrier before comparing) plus a per-GPU-generation
  `hw_isidle()` register check. `adreno_spin_idle()` polls this with a real
  timeout, and `adreno_idle()` requires the device mutex held first
  (guaranteeing no new submissions race the check). This is **direct
  hardware evidence the AP can verify itself** — no remote-firmware trust
  needed, unlike the DSP case. The strongest signal found so far.
- **UFS (`ufshcd.c`)**: `ufshcd_wait_for_doorbell_clr()` reads
  `REG_UTP_TASK_REQ_DOOR_BELL` / `REG_UTP_TRANSFER_REQ_DOOR_BELL` — real MMIO
  registers on the UFS host controller, not the software `outstanding_reqs`
  bitmap — polled with a real timeout, after `ufshcd_hold_all()` blocks new
  work. Same strength as GPU: direct, self-verifiable, no round-trip trust
  required.

### Running tally across all subsystems checked

| Subsystem | Verdict | Why |
| --- | --- | --- |
| GPU | Strong | Direct ringbuffer rptr/wptr + hw_isidle register read |
| UFS | Strong | Direct doorbell register read |
| ADSP/CDSP/SLPI | Good | Synchronous firmware handshake (`sysmon_send_shutdown`), checkable return code |
| Camera | Weak | Real close() path exists; no independent idle/done check found |
| SMMU | Blocked | Real debug registers exist; bit encoding undocumented in this tree |
| USB, SyncBoss, other-DMA, secure/TZ, AOP | Unexamined | 5 of 10 subsystems not yet looked at |

3 of 10 subsystems now have genuinely trustworthy completion evidence to
build real backends against. This is real, continuing progress — still far
from a working end-to-end shutdown sequence, and nothing has been run
against real hardware.

### Remaining five subsystems checked — full survey now complete

- **USB (dwc3, `gadget.c`)**: `dwc3_gadget_run_stop()` writes `DCTL_RUN_STOP`
  then polls the real `DWC3_DSTS_DEVCTRLHLT` hardware bit with a timeout —
  same direct-register tier as GPU/UFS. Explicitly stops active transfers
  first per the hardware databook's documented requirement, not assumed.
- **SyncBoss (`syncboss_spi_core.c`)**: `stop_streaming_locked()` sends a
  graceful "go to sleep" MCU command (`shutdown_syncboss_mcu_locked`), then
  `kthread_stop()` on the worker thread that drives the actual SPI
  transfers — a real, unconditional blocking kernel join, not a poll with a
  timeout that can give up early. Good evidence, though queued-but-unsent
  userspace messages are discarded rather than drained (acceptable for DMA
  safety, not for data completeness).
- **Secure/TZ (`qseecom.c`)**: `__qseecom_unload_app()` makes a real
  synchronous SCM call into TrustZone and can return `-EBUSY` — a genuine
  "didn't actually finish" signal the caller correctly handles by restoring
  the refcount and refusing to free memory. Same tier as ADSP: trustworthy,
  but it's trust in another security world's response, not direct AP-side
  register evidence.
- **Video/Venus (likely what "other DMA" covers, `venus_hfi.c`)**:
  `venus_hfi_core_release()` sets state and unloads firmware under a mutex,
  but doesn't itself show a synchronous wait for outstanding session
  completion — that responsibility seems to sit with callers via
  `session_clean`, not verified in depth here. Weaker and less certain than
  the ADSP/qseecom pattern; flagged as needing more investigation before
  trusting it, not ruled out.
- **AOP/AOSS (`reset-qcom-aoss.c`)**: genuinely weak. `qcom_aoss_control_*`
  is a plain register write followed by a **fixed `usleep_range(200, 300)`**
  with a comment trusting Qualcomm's own "6 32kHz sleep cycles" hardware
  timing spec — no readback, no confirmation, same category as camera's
  unverified close(). (Bonus found here: AOSS also exposes per-subsystem
  reset lines for CAMSS/VENUS/GPU/DISPSS/WCSS/LPASS — a second, more blunt
  reset mechanism separate from the graceful PIL/subsystem_restart path,
  worth remembering if the graceful path ever needs a fallback.)

### Full ten-subsystem survey (final)

| Subsystem | Verdict | Why |
| --- | --- | --- |
| GPU | Strong | Direct ringbuffer rptr/wptr + hw_isidle register read |
| UFS | Strong | Direct doorbell register read |
| USB | Strong | Direct DWC3_DSTS_DEVCTRLHLT register read |
| ADSP/CDSP/SLPI | Good | Synchronous firmware handshake, real return code |
| SyncBoss | Good | Real kthread_stop() join + graceful MCU command |
| Secure/TZ | Good | Synchronous SCM call, real -EBUSY error path |
| Camera | Weak | Close() path exists; no independent idle check |
| Video/other-DMA | Weak/unclear | No synchronous wait found in core_release itself; needs more digging |
| AOP/AOSS | Weak | Fixed-delay only, zero hardware confirmation |
| SMMU | Blocked | Real debug registers exist; bit encoding undocumented |

**Honest summary:** 6 of 10 subsystems now have real, trustworthy
completion evidence (3 strong direct-register, 3 good synchronous-handshake)
to build real `qkx_shutdown_backend` implementations against. 3 are weak —
their existing stop paths are real but give no independent confirmation,
meaning any backend built on them would be trusting rather than verifying.
1 (SMMU) is blocked on missing vendor documentation. This is a complete,
honest first pass across every subsystem the shutdown framework enumerates
— still zero lines of real backend code written, and nothing has touched
actual hardware.

### SMMU re-investigated — not actually blocked, wrong layer checked first

The vendor TBU/TCU test-bus registers (`arm-smmu-debug.c`) really are
undocumented and unusable. But that's the wrong layer to look at. The ARM
SMMU **architecture itself** (standard, publicly documented — not Qualcomm
proprietary) defines a real completion mechanism: TLB sync + status
registers.

`__arm_smmu_tlb_sync()` in `arm-smmu.c` writes to the sync register
(`ARM_SMMU_CB_TLBSYNC` per-context, or `ARM_SMMU_GR0_sTLBGSYNC` globally),
then polls the matching status register's `GSACTIVE` bit with a real,
bounded, exponential-backoff timeout — returning confirmed success or a
real failure, not a guess. This is the exact same architectural mechanism
every ARM SMMU driver in mainline Linux uses, already wired into the
standard `arm_smmu_unmap()` → `io_pgtable_ops` → `tlb_sync` callback chain
that fires on every ordinary `iommu_unmap()` call. No new SMMU-specific
code would need writing — this path already exists, is already tested, and
runs on every normal unmap today.

**The honest caveat, and why SMMU isn't independently solvable:** TLB sync
confirms the SMMU's translation caches are now clean of whatever was just
unmapped — it does **not**, by itself, prove no DMA master was using that
mapping a moment before. A transaction that started just before the unmap
could still be mid-flight. This means SMMU safety isn't a standalone
seventh/eighth problem — **it's entirely downstream of correctly stopping
each master first**, using the per-master signals already surveyed (GPU/
UFS/USB's direct registers, ADSP/SyncBoss/TZ's handshakes). Get the master
stop order right, confirmed via those signals, *then* `iommu_unmap()` +
TLB sync gives real, architectural, already-tested confirmation that the
SMMU side is clean too.

**Revised verdict: SMMU moves from Blocked to Good** — same tier as the
synchronous-handshake subsystems, with the explicit dependency noted that
its correctness rides on the master-stop ordering, not on anything SMMU-
specific.

### Updated full tally

| Subsystem | Verdict |
| --- | --- |
| GPU, UFS, USB | Strong |
| ADSP/CDSP/SLPI, SyncBoss, Secure/TZ, **SMMU** | Good |
| Camera, Video/other-DMA, AOP/AOSS | Weak |

7 of 10 subsystems now have real, trustworthy completion evidence — up from
6. SMMU was never the hardest piece; it was just the wrong register file to
check first.

### Re-checked the 3 weak subsystems — one real upgrade, one partial, one confirmed weak

- **Video/Venus — upgrades to Good.** Was looking at the wrong function
  (`core_release`) last pass. `msm_comm_session_abort()` in
  `msm_vidc_common.c` sends the HFI abort command then does a real
  `wait_for_completion_timeout()` on `HAL_SESSION_ABORT_DONE` — a genuine
  firmware round-trip with a bounded timeout, same tier as the ADSP/TZ
  handshakes. Video moves from "weak/unclear" to **Good**.
- **AOP — partially upgrades, with an important caveat.** The real AOP
  message transport (`drivers/mailbox/msm_qmp.c`, distinct from the
  `reset-qcom-aoss.c` reset-controller checked earlier) is genuinely
  IRQ-driven: `qmp_send_data()` reads back the message-RAM write to confirm
  it landed, sets a busy flag preventing overlapping sends, fires a real
  hardware IRQ, and schedules a real TX timeout — with `wait_for_completion
  _timeout()` elsewhere in the driver for link/channel-ready handshaking.
  This is a real, checkable transport, not a blind delay. **But**: this
  only confirms a message was *delivered and acked* — whether any specific
  "quiesce your DMA-adjacent activity" message is even defined for AOP, or
  whether AOP is a meaningful DMA master to begin with (it's a power-
  sequencing coprocessor, not a bus master like the others), is still an
  open question. The *reset-controller* path (`reset-qcom-aoss.c`, fixed
  200-300µs delay, zero confirmation) is a separate, still-weak mechanism
  that would still be the one actually touching CAMSS/VENUS/GPU/WCSS/LPASS
  reset lines directly. Net: the messaging transport is trustworthy; what
  you'd actually use it to confirm, for this specific problem, is unclear.
  AOP stays flagged as needing real scoping before it can be called solved.
- **Camera — confirmed weak, even at the hardware-block level.** Checked
  below the `cam_req_mgr_close()` layer: `cam_ife_csid_halt_csi2()` (the
  actual CSI2 receiver hardware halt) is pure fire-and-forget register
  writes — disable IRQ mask, zero two config registers, done. No readback,
  no status poll, nothing. This is the same shape as AOSS's fixed-delay
  reset, just without even the delay. Checked the layer most likely to
  have a GPU/UFS/USB-style direct register confirmation, and it genuinely
  isn't there. Camera remains the weakest subsystem with no plausible
  nearby fix found.

### Final tally

| Subsystem | Verdict |
| --- | --- |
| GPU, UFS, USB | Strong — direct register read |
| ADSP/CDSP/SLPI, SyncBoss, Secure/TZ, SMMU, **Video/Venus** | Good — synchronous handshake |
| **AOP** | Mixed — real transport, unclear what to confirm with it |
| **Camera** | Weak — confirmed at both layers checked, no completion signal exists |

8 of 10 subsystems now have real, trustworthy completion evidence. AOP needs
scoping work (what message, if any, actually matters here) rather than more
register-hunting. Camera is the one subsystem where the honest conclusion is
that nothing better was found — not because the search was shallow, but
because the hardware genuinely doesn't expose it at either layer checked.

### Camera re-investigated further — real evidence found, earlier verdict was wrong

Went one layer deeper than CSID (the CSI *receiver*, which only decodes
incoming camera-sensor data) to VFE/IFE — the block that actually does the
DMA *writes* to memory, which is what we actually care about for teardown
safety. This changes the picture completely.

`cam_vfe_reset()` in `cam_vfe_core.c`:
1. Subscribes to a real hardware IRQ (`cam_vfe_reset_irq_top_half`), with a
   hardware-version-specific register bitmask for the actual top-reset
   status bits (`CAM_VFE_48X_TOP_RESET_MASK` etc — real silicon, not
   software bookkeeping).
2. Triggers the hardware reset via `vfe_top->hw_ops.reset(...)`.
3. Blocks on `wait_for_completion_timeout(&vfe_hw->hw_complete, 500)` —
   genuinely waiting for the hardware interrupt to fire and call
   `complete()`, with a real, distinguishable timeout path
   ("Reset Timeout" logged, not silently assumed success).

And this isn't buried in an error-only path: `cam_ife_mgr_reset()` (which
resets both CSID and VFE for a context, calling this exact function) is
formally registered as `hw_mgr_intf->hw_reset` — a standard, intentional
operation in the ISP hardware manager's own interface, not something only
reachable during overflow/error recovery.

**This means camera's DMA-relevant hardware (VFE) has the same tier of
evidence as GPU/UFS/USB — a real, interrupt-backed, timeout-bounded
hardware confirmation, reachable through a normal, exposed API.** My
earlier "confirmed weak" verdict was wrong — I'd checked the request-
manager's software refcount (`cam_req_mgr_close`) and the CSID receiver's
halt (fire-and-forget, correctly weak), but missed VFE, which is the block
that actually matters for DMA safety and turns out to be solid.

**Revised verdict: Camera moves from Weak to Strong.**

### Updated final tally

| Subsystem | Verdict |
| --- | --- |
| GPU, UFS, USB, **Camera (VFE)** | Strong — direct register/IRQ-backed hardware confirmation |
| ADSP/CDSP/SLPI, SyncBoss, Secure/TZ, SMMU, Video | Good — synchronous handshake |
| AOP | Mixed — real transport, unclear what message matters |

9 of 10 subsystems now have real, trustworthy completion evidence. Only AOP
remains genuinely unresolved, and that's a scoping question (what, if
anything, needs confirming from it) rather than a missing-register problem.

## Does kexec/the kernel actually crash without AOP work? — real answer found

Went looking for the actual mechanism, not just "is AOP a DMA master." Found
it in `drivers/soc/qcom/rpmh-rsc.c` — the RPMh/TCS (Trigger Command Set)
driver that every other subsystem's clock/regulator/power-rail requests
flow through to reach AOP. This is foundational infrastructure, not a
peripheral.

**The real risk, concretely:**

`tcs_is_free()` checks a pure **software** bitmap (`drv->tcs_in_use`), not
any hardware register. `rpmh_rsc_probe()` unconditionally runs
`bitmap_zero(drv->tcs_in_use, MAX_TCS_NR)` on every probe — it assumes
every TCS hardware slot is free, with **zero verification against real
hardware state**. That assumption is correct on a true cold boot, where
XBL/PBL has just brought the SoC up clean. It is **not** necessarily true
after a kexec jump, where AOP itself was never reset and its hardware TCS
queues kept running exactly as the old kernel left them.

`rpmh_rsc_send_data()`'s own doc comment confirms the exposure window:
"blocks until a valid data is written to the TCS" — written, not
completed. Actual completion is a separate, asynchronous `tcs_tx_done` IRQ.
So at the moment of a kexec jump, it's entirely possible for an
ACTIVE_TCS slot to hold a command AOP is still actively executing, not yet
tx-done-acknowledged. If the new kernel's blindly-zeroed bitmap lets it
reuse that same physical slot, that's a real race — corrupting or losing
whichever rail/clock request was in flight. Since TCS carries voltage/
clock/power-rail requests, the failure mode genuinely ranges from subtle
misbehavior to a hard hang or crash depending on which rail is hit (e.g. a
corrupted CPU core-voltage request).

**This is why AOP matters, and it's not the "is this even a DMA master"
question I originally framed it as.** It's not about moving data — it's
about a shared hardware command queue that the new kernel's own driver
blindly assumes is empty.

**The good news: this is independently checkable and already has the
right primitives in-tree.**
- `RSC_DRV_STATUS` (0x18) and `RSC_DRV_CMD_ENABLE` (0x1C) are real, directly
  readable **hardware** registers per TCS — not software bookkeeping. Same
  tier of evidence as GPU/UFS/USB's direct-register checks.
- `rpmh_rsc_invalidate()` already exists and is already used today (called
  from `rpmh.c` during the normal suspend/resume flush path) to safely
  invalidate SLEEP_TCS/WAKE_TCS entries — real, tested, in-tree code, not
  something to invent.
- What's missing: the same real-register check extended to ACTIVE_TCS
  before a jump, and doing this across every RSC instance on the SoC
  (there's more than one — apps_rsc is the one we've wired in our own
  device tree, there may be a disp_rsc too), not just the one obvious one.

### Revised AOP verdict

Moves from "Mixed/unclear" to **Strong** — real hardware registers exist to
verify TCS state directly, and the existing `rpmh_rsc_invalidate()` covers
most of what's needed already. The honest answer to "will it crash without
this": **yes, plausibly** — not a guess, but via a specific, named,
independently-verifiable mechanism (stale TCS slot reuse), not a vague
hand-wave. This is exactly the kind of thing that stays invisible on every
normal reboot (because XBL resets AOP's hardware state too) and only
becomes a real problem the moment a kexec jump skips that reset — which is
the whole reason this research mattered.

### Final-final tally

| Subsystem | Verdict |
| --- | --- |
| GPU, UFS, USB, Camera, **AOP** | Strong — direct hardware register confirmation |
| ADSP/CDSP/SLPI, SyncBoss, Secure/TZ, SMMU, Video | Good — synchronous handshake |

All 10 subsystems now have a real, evidenced answer. None are unexamined
or blocked. Still zero backend code written against this framework, and
nothing has touched real hardware — this is a complete map of what such
code would need to do and why, not the code itself.

## Real backend implementation: all 10 subsystems, compiled and verified

Implemented real `qkx_shutdown_backend` structs for all 10 subsystems in
a new `backends/` kernel module (`qkx_backends.ko`), built against this
exact device's real, already-built kernel (`android-out/`, vermagic
`4.19.325-cip128-st12 ... aarch64` - a genuine match, not assumed).

**What's real and what's honestly scoped down:**

Every backend's `.inspect` populates a real, schema-correct
`qkx_shutdown_inventory` (capabilities drawn from the real `qkx_shutdown_step()`
action requirements per subsystem, not guessed). Every backend's evidence
computation in `qkx_evidence_for()` (`qkx_common.h`) exactly replicates
shutdown.c's own static, unexported `qkx_shutdown_evidence()` formula -
checked against its real source, not reinvented.

- **GPU, UFS, USB, Camera, AOP** - direct, read-only MMIO register checks
  at physical addresses confirmed from this device's own device tree
  (`kona.dtsi`/`kona-usb.dtsi`/`kona-camera.dtsi`/`kona.dtsi`'s
  `apps_rsc`/`disp_rsc`), at register offsets confirmed from the real
  driver source read earlier this session (A6XX ringbuffer rptr/wptr +
  RBBM_STATUS; UFS doorbell registers; DWC3_DSTS; VFE top IRQ status;
  RSC_DRV_CMD_ENABLE across every TCS slot on both RSC instances).
  Camera's check is honestly weaker than the others: it does not trigger
  the real IRQ-backed `cam_vfe_reset()` path (that would race the actual
  owning driver if it's still loaded), only observes the top status
  register passively.
- **ADSP/CDSP/SLPI** - uses the real, exported `find_subsys_device()` +
  `subsys_get_crash_status()`. Honestly weaker than register-tier: this
  confirms "not crashed," not "idle" - no exported idle query exists.
- **SyncBoss, Video** - no exported symbols exist in either driver
  (confirmed directly, not assumed) for an outside module to query. Both
  use the same real, generic, exported signal instead: the device's own
  runtime-PM suspended state, via `bus_find_device_by_name` +
  `pm_runtime_status_suspended()`. Honestly the weakest-but-still-real
  tier, clearly short of what their own internal mechanisms (kthread_stop,
  HAL_SESSION_ABORT_DONE) would give if called from inside the driver.
- **SMMU, Secure/TZ** - implemented as deliberate, documented pass-throughs.
  SMMU's real safety property is downstream of the other 9 backends (this
  session's own research conclusion); Secure/TZ's real completion signal
  (`qseecom_shutdown_app`'s `-EBUSY` path) requires owning a TZ app handle
  this module never opens, and forcing someone else's handle closed would
  be wrong, not safe.

**Every backend is read-only.** Grepped the whole directory for
`writel`/`iowrite`: zero matches. Nothing here can alter hardware or
driver state - at most it reads registers and kernel-exported status, the
same access pattern `arm-smmu-debug.c` itself uses for its own diagnostics.

**Real build problems hit and fixed, not glossed over:**
- `modules_prepare` against a from-scratch `.config` failed twice on
  genuine host-toolchain/kernel-version mismatches (GCC's `__has_attribute`
  handling in vdso code; clang rejecting an old KGDB inline-asm immediate).
  Switched to building against the kernel's own already-built output
  (`android-out/`, vermagic-matched) instead of re-deriving from scratch.
- First real build attempt failed kernel-tree-wide on
  `register unsigned long current_stack_pointer asm ("sp")` - fixed by
  adding `CROSS_COMPILE=aarch64-linux-gnu-` to the module build invocation
  (clang needs the explicit target triple this derives; without it, the
  arm64 global-register-variable declaration in `stack_pointer.h` doesn't
  resolve on this host).
- `find_subsys_device()` returns `struct subsys_device *`, not
  `struct device *` - a real type mismatch caught by the compiler, fixed.
- The build's own "unused variable" warning on `qkx_shared_ctx` caught a
  real bug: every backend's `.context` field was never actually wired up
  in `module_main.c`, which would have made every `ctx->client_id`/
  `*ctx->epoch` dereference a NULL-pointer read the instant the real
  orchestrator called into any backend. Fixed by wiring `.context` for
  all ten backends in `qkx_backends_init()`.

**Final state: `qkx_backends.ko` builds clean, zero warnings, real ELF
ARM64 kernel module, vermagic-matched to this device's actual kernel.**
Not yet loaded or run on real hardware - that's the next real step, and a
materially different risk category from everything checked so far tonight
since it means running new code inside the one kernel this headset has.

## First successful real-hardware test, after two real crashes and fixes

With the raw-MMIO-read removal in place, `qkx_backends.ko` loaded cleanly
on the real device (insmod exit 0), ran its self-test across all 10
backends, unloaded cleanly (rmmod exit 0), and caused no reboot. Real
output from dmesg, with the headset actively on and in use over USB at
the time:

| Subsystem | Result | Consistent with reality? |
| --- | --- | --- |
| Camera | idle | Yes - not capturing |
| ADSP/CDSP/SLPI | idle | Not crashed |
| GPU | **busy** | Yes - actively rendering Home UI |
| SyncBoss | idle | Point-in-time PM snapshot between SPI transactions |
| USB | **busy** | Yes - live ADB/USB session in progress |
| UFS | **busy** | Plausible under active use |
| Video | idle | Not transcoding |
| SMMU | idle | Pass-through (by design) |
| Secure/TZ | idle | Pass-through (by design) |
| AOP | idle | Pass-through (by design) |

GPU/USB/UFS correctly reporting "busy" while the device is actively being
used, rather than crashing or falsely claiming idle, is exactly the
honest behavior this revision was aiming for.

### Honest standing after tonight's full arc

Two real reboots were caused by this research, both understood, both
fixed by removing the specific unsafe operation rather than patching
around it repeatedly. The module now:
- Never performs a raw MMIO register read anywhere in any backend.
- Gives real, consistent, hardware-grounded answers for 7 of 10
  subsystems (idle/busy via clock-gate state or a safe exported query).
- Explicitly, honestly defers on 3 (SMMU, Secure/TZ, AOP) rather than
  guess - including AOP, the subsystem whose finding mattered most, which
  remains a real, open, unresolved risk, not a disproven one.

This is a materially weaker evidence tier than originally designed, but
it is the tier that survives contact with real hardware without crashing
it a third time. All of this logged and pushed to the repo as it
happened, including the mistakes.

## Searching for UFS-equivalent safe mechanisms on GPU/USB/Camera

The UFS fix worked because `ufshcd_hold()`/`ufshcd_release()` is a clean,
minimal, already-exported primitive: find the device, get the pointer
Linux already stored for it, hand that pointer straight back to the real
driver's own synchronization function, done. Looked for the same shape
elsewhere.

- **GPU (KGSL)**: `kgsl_active_count_get()`/`kgsl_active_count_put()` exist
  in `kgsl_pwrctrl.c` and look exactly like what's needed - but neither has
  an `EXPORT_SYMBOL` anywhere (checked directly). No external module can
  call them. No safe mechanism available; GPU stays at the no-raw-read
  tier.
- **USB (dwc3-msm.c)**: only narrow, endpoint-specific exports exist
  (`msm_ep_config`, `msm_dwc3_reset_dbm_ep`, `msm_dwc3_reset_ep_after_lpm`)
  - these manage the DBM (bulk-transfer acceleration block)'s endpoints,
  not a general "ensure the controller is genuinely awake" primitive. No
  safe mechanism available; USB stays at the no-raw-read tier.
- **Camera (CPAS)**: `cam_cpas_reg_read()`/`cam_cpas_reg_write()` are real
  and exported - genuinely promising at first glance, since CPAS is the
  camera subsystem's central power/bus-vote arbiter and routes register
  access through its own `process_cmd` hardware command path, which
  presumably handles any necessary sequencing internally. But unlike
  `ufshcd_hold()`, this requires a `client_handle` obtained via
  `cam_cpas_register_client()` first - becoming an actual registered
  client of the camera subsystem's resource-management framework, with
  callback handling, not simply borrowing an existing device pointer.
  This is a different risk category: a UFS-style hold/release call only
  observes; registering as a CPAS client makes this module an active
  participant in real bus-bandwidth voting and camera power sequencing,
  with more surface area for something to go wrong, and with unclear
  interaction if the real camera driver is itself simultaneously
  registered. Not pursued further this session - flagged as a real,
  possible future direction, not a quick safe win like UFS was.

### Honest standing

UFS is the only subsystem of the five register-based backends with a
genuinely safe, minimal, already-exported path back to strong evidence.
GPU and USB have no such path - the relevant primitives either don't
exist or aren't exported. Camera has a real path, but it is a heavier,
differently-risky integration than tonight's crash history makes
advisable to attempt without more dedicated care.

## CPAS pursued further - confirmed dead end for Camera's actual need

Checked `enum cam_cpas_reg_base` (the only parameter selecting which
register space `cam_cpas_reg_read()`/`cam_cpas_reg_write()` can reach):
only three values exist - `CAM_CPAS_REG_CPASTOP`, `CAM_CPAS_REG_CAMNOC`,
`CAM_CPAS_REG_CAMSS`. Traced `CAM_CPAS_REG_CAMSS` to
`camss_top/cam_camsstop_hw.c` - a shared, top-level wrapper register
block for the camera subsystem as a whole, confirmed structurally
distinct from VFE0's own per-block register space (the same
top-wrapper-vs-per-block split already seen between CSID's top registers
and VFE's own `cam_vfe_core.c`). `cam_cpas_reg_read()` cannot reach
`QKX_VFE_IRQ_STATUS0` at all, registered client or not.

This means the heavier CPAS client-registration integration flagged last
entry would not have solved the actual problem even if built - it reads
the wrong address space entirely. Checked the rest of
`techpack/camera/drivers/cam_isp/` for any other exported symbol: the
only one found is `cam_ife_csid_hw_probe_init`/`_deinit` in the CSID
driver - internal probe/remove lifecycle hooks, unsafe to call from
outside the driver's own loading sequence (would attempt to
re-initialize hardware the real driver already owns).

### Final conclusion for Camera

No safe, externally-callable path to VFE0's register exists anywhere in
this driver tree. Camera correctly remains at the clock-gate-only, no-
raw-read tier, same as GPU and USB. UFS's fix was a genuine exception
this session found a real path for, not a pattern that generalizes to
every subsystem - confirmed by actually trying, not assumed.

## Camera, exhausted from every angle tried

After GPU and USB both found real signals via generic framework-level
sysfs (gpubusy, UDC state) rather than driver-specific APIs, tried the
same strategy for Camera. Checked, on the real device, in order:

- **V4L2/media class devices** (`/sys/class/video4linux/`): 13 subdevs
  exist (cam-cpas, cam-isp, cam-isp-fastpath, 6x cam-csiphy-driver, 4x
  cam-sensor-driver) plus video0/1/32/33. Every one of them links back to
  the same parent device (`soc:qcom,cam-req-mgr`) and every one of their
  own `power/runtime_status` files reads "unsupported" - these are
  virtual registration nodes under the request-manager framework, not
  independently power-tracked devices. No distinct per-VFE0 signal here.
- **devfreq class** (`/sys/class/devfreq/`): real entries exist for UFS,
  GPU, several CPU/NPU bandwidth monitors - no camera-specific node
  registered at all. Not available for this subsystem.
- **tracefs** (`/sys/kernel/tracing/`): genuinely mounted and real (unlike
  debugfs), with real generic PM tracepoints available (`clock_enable`,
  `clock_disable`, `power_domain_target`, `suspend_resume`, etc). Not
  pursued further: this is event history, not a queryable current state -
  answering "is VFE busy right now" from a trace buffer would mean
  enabling tracing, capturing a window, and interpreting recency, a
  fundamentally different and much fuzzier tool than a direct state read
  like gpubusy or UDC state.
- **CPAS** (previous round): confirmed reads the wrong address space
  entirely (CPASTOP/CAMNOC/CAMSS-top, not VFE0's own registers).
- **debugfs** (previous round): not mounted on this device at all, so
  `sessions_info` and any other debugfs-based signal was never reachable
  regardless of what it exposes.
- **Direct exports** (earlier round): only
  `cam_ife_csid_hw_probe_init`/`_deinit`, internal lifecycle hooks unsafe
  to call from outside the driver's own loading sequence.

### Final conclusion

This was a genuinely thorough search across every category of signal
that worked for a sibling subsystem (framework-level sysfs, driver-
specific exports, debugfs, bus-level APIs, trace infrastructure) - not a
quick look. Camera has no safe externally-reachable signal beyond the
clock-gate check already in place, and stays honestly at that tier. GPU
and USB's wins don't generalize here; this is a real, checked limit, not
an assumption.

## Checking for prior art - WitherBlitz's own repo has much deeper work

Went looking for whether anyone else has tackled this. Found real,
substantial prior art inside the same upstream repo this whole kexec
effort is based on (`Quest-2-Steam-OS-Dual-Boot`), in files not
previously examined this session:

- **`module/camera_dsp_diag.c`** - a standalone, read-only "advisory
  snapshot" module, far more sophisticated than tonight's work:
  - Gates on the exact kernel release string
  (`4.19.325-cip128-st12-g49638c7a8637`) plus device-tree model/compatible
  checks before doing anything.
  - Hash-verifies the actual compiled bytes of a known exported function
  (`cam_req_mgr_is_open`, hashing its first 68 bytes and comparing
  against a known-good constant) *before* trusting any pointer-offset
  math built against private struct layouts - a genuinely rigorous
  answer to the struct-ABI-mismatch risk flagged earlier tonight for our
  own `ufshcd_hold()` use.
  - Reads private kernel structures directly via raw pointer offsets
  from that verified symbol's address, protected by `probe_kernel_read()`
  (returns an error instead of crashing on a bad address) and
  non-blocking `mutex_trylock()` everywhere - never blocks, reports
  "camera-busy"/"core-busy" and bails gracefully instead.
  - Walks the real CRM session/link list, workqueue pending/free/task
  counts, and queue read/write/applied indices - genuine, deep software
  state, not a register.
- **`QUEST2_OBJECT_CUSTODY.md`** and siblings describe a much heavier,
  separate effort: actually patching and recompiling `cam_req_mgr` and
  FastRPC's own source to add reference-counted custody tracking that
  doesn't exist upstream, gated behind `CONFIG_QKX_OWNER_GATE`
  (default off, requires camera/FastRPC built statically in - a
  fundamentally different strategy than an external observing module).

### The important finding: even this hasn't solved it

Their own `QUEST2_RELEASE_READINESS.md` audit states plainly:
"**Physical shutdown, DMA quiescence and recovery remain unvalidated.**"
Stage 2 of their own 9-stage audit lists proving camera/DSP/GPU/USB/UFS/
AOP DMA completion as an open, unresolved blocker - "No complete
shutdown has been observed." This is independent confirmation that
tonight's conclusions (Camera has no safe externally-reachable signal;
GPU/USB needed real digging to find one at all; AOP's core finding
remains open) line up with the most advanced known public effort on
this exact device and kernel, not a sign we were missing something
obvious.

One technique caveat, not to overclaim: `probe_kernel_read()` guards
against bad *virtual* addresses (page faults). Tonight's real crashes
were SError *bus* faults from power-gated *physical* MMIO - a different
failure mode that primitive likely would not have caught. The
hash-anchor verification and trylock-everywhere design are the parts
genuinely worth adapting; probe_kernel_read doesn't directly cover what
actually crashed this session.

## Checked Secure/TZ for the same upgrade - genuinely blocked, not just hard

After upgrading 8 of 10 backends tonight with real BTF-verified private
struct reads (Camera, SyncBoss, Video, ADSP, GPU, USB, AOP-informational,
SMMU-informational), checked whether qseecom could get the same
treatment.

`static struct qseecom_control qseecom;` in qseecom.c (confirmed
directly) - a plain static global, exactly like camera's g_dev, with
zero dev_set_drvdata()/platform_set_drvdata() calls anywhere in the
file. The only way to reach it from outside is the same kind of
function-relative-offset trick used for camera's g_dev.

The real difference from camera: WitherBlitz had already done that
specific reverse-engineering work and published a testable offset
(cam_req_mgr_is_open + 0x12d8c00) with a known-good anchor hash we could
independently verify against. For qseecom, no such prior art exists -
there is no known offset to test, which means computing one now would
mean blind disassembly of this device's own vmlinux with no independent
way to confirm correctness. That's a fundamentally different risk than
everything else attempted tonight: every other upgrade either used a
safe, standard dev_get_drvdata() path, or (for camera) had a specific,
externally-verifiable number to check before trusting it. Guessing a
new offset blind would mean trusting our own reverse-engineering with no
cross-check at all - precisely the kind of unverified assumption that
caused two real crashes earlier this session.

### Conclusion

Secure/TZ remains a pass-through. Not for lack of trying, and not
because the data doesn't exist (registered_app_list_head is real,
useful information) - but because reaching it safely would require
genuinely new reverse-engineering work this session doesn't have the
tools or time to independently verify, unlike every other upgrade
tonight which did.

## Secure/TZ re-checked directly against Meta's own source - confirmed, not assumed

Went back to Meta's own qseecom.c source specifically to check for
anything missed, rather than relying on the earlier conclusion alone.

- `qseecom_probe()` only calls `qseecom_init_dev(pdev)` - read its full
  body directly. It sets `qseecom.dev = &pdev->dev` (the static global
  storing a pointer TO its device) but never the reverse
  (`dev_set_drvdata()` is never called anywhere in the file, confirmed
  by grep across the whole source, not just the probe function). The
  pointer only flows one direction - global to device, never device to
  global - so there genuinely is no drvdata path, checked from both
  directions this time.
- Checked the real, live device directly: `82400000.qseecom` exists on
  this headset (confirmed via `/sys/bus/platform/devices/`), but its
  sysfs directory holds only the generic platform-device attributes
  every device gets (driver symlink, modalias, power/, uevent) - nothing
  qseecom-specific exposed.
- Checked whether a smaller, differently-reachable struct could help
  instead: `struct qseecom_handle` (the per-app session handle passed to
  qseecom_start_app/send_command/shutdown_app) is just three fields -
  `dev`, `sbuf`, `sbuf_len`. Even if some other properly drvdata-attached
  driver held one as part of its own state, it carries no information
  about overall TZ app activity (no app count, no list) - this side door
  wouldn't have told us anything useful even if it existed.

### Conclusion, now source-confirmed rather than inferred

Secure/TZ's pass-through stands, checked thoroughly against Meta's own
published source from multiple angles (probe function, live device
sysfs, the handle struct's own shape) - not something to keep digging at
with tonight's toolkit. The gap is real and specific: no drvdata path
exists, and no externally-verifiable offset (the kind camera's
pre-existing prior art provided) exists either.

### qseecom_handle side-door search, completed - confirmed dead end

The broader search across drivers/+techpack/ for other qseecom_handle
holders (started before the conclusion above, finished after) found
exactly three hits: `msm_camera_tz_util.c`, `mpq_sdmx.c` (DVB demux),
and `msm_camera_tz_i2c.c` - all three in the legacy `camera_v2` driver
tree. Confirmed directly against this device's own captured config:
`CONFIG_MSM_CAMERA is not set` - camera_v2 isn't compiled into this
kernel at all (Quest 2 uses techpack/camera, already confirmed via its
real v4l-subdev names). No matching device nodes exist live either.
This closes the side-door search cleanly: nothing on this specific
device holds a reachable qseecom_handle, confirmed rather than assumed.

## Real action testing: complete coverage, and an honest line about what it does and doesn't prove

Extended beyond observation-only tonight, methodically and carefully:

| Subsystem | Real action tested | Mechanism |
| --- | --- | --- |
| ADSP, CDSP | acquire + immediate release | `subsystem_get()`/`subsystem_put()` |
| Venus | acquire + immediate release | `subsystem_get()`/`subsystem_put()` |
| a650_zap (GPU-adjacent) | acquire + immediate release | `subsystem_get()`/`subsystem_put()` |
| SPSS (secure-adjacent) | acquire + immediate release | `subsystem_get()`/`subsystem_put()` |
| WLAN, NPU, CVPSS | acquire + immediate release | `subsystem_get()`/`subsystem_put()` |
| UFS | acquire + immediate release | `ufshcd_hold()`/`ufshcd_release()` |

Every one of these tested on real hardware, each at least twice (initial
run plus an unload/reload to confirm identical, stable results), zero
crashes, nothing concerning in dmesg, device fully responsive throughout
every single test.

### The honest line this round does NOT cross

Every test above only ever acquires a reference this module did not
previously hold, then immediately releases it again - the subsystem's
real refcount never actually reaches zero because of anything this
module did (other legitimate holders keep it above zero throughout).
This proves the *mechanism* is safe to call - the API itself, the
locking, the module refcounting - but it does **not** prove what
happens when a refcount actually reaches zero and `subsys_stop()`'s real
teardown sequence (`sysmon_send_shutdown()` + the PIL `shutdown()`
callback) actually runs. That is the specific thing `qkx_dma_gate()`'s
refusal is actually about, and it remains genuinely untested by
anything in this round.

Same honest gap for UFS: `ufshcd_hold()`/`ufshcd_release()` is a
wake-and-keep-awake primitive, not a stop primitive - proving it's safe
to call doesn't say anything about UFS's own teardown path.

### Where this leaves confidence toward the gate

Real, valuable progress: the acquire/release *mechanics* for 10 real
targets are now proven safe on this exact hardware, not assumed. But
confidence in the actual gate decision needs confidence in the
*teardown* path specifically, which is a materially bigger, more
consequential step - it would mean actually invoking a real subsystem
stop, not bracketing one. That decision deserves its own explicit
discussion before attempting, not a natural continuation of this round's
pace.

## Researching the real subsys_stop() path before attempting it

Checked whether any of the 9 subsystem_get/put tests this session
accidentally exercised a real boot/stop cycle, by checking for
subsys_start()'s own real log line ("Powering up %s", plain pr_info(),
confirmed in subsystem_restart.c). None appeared on a fresh reload -
confirmed, not assumed: every one of those 9 subsystems was already
running before this module ever touched it. None of this session's
action tests have exercised the real teardown path.

Looked for a safer, non-destructive way to probe the shutdown
communication channel itself without sending a real shutdown.
sysmon_get_reason() (sysmon-qmi.c, EXPORT_SYMBOL) looked promising - a
read-only "get failure reason" QMI query - but it reaches out to real
remote firmware over QMI, a materially different and less
locally-verifiable risk than anything tested so far (every earlier
action test this session stayed entirely in local kernel bookkeeping).
Its semantics are also for querying a subsystem AFTER a crash; calling
it on a healthy subsystem is genuinely uncertain territory this session
doesn't have grounds to resolve from source alone. Not attempted.

Instead, checked WitherBlitz's own independent research
(docs/QUEST2_OWNER_AUDIT.md) for prior findings on this exact path -
real, valuable cross-validation, not something to attempt blind:

- Confirmed the same gap this session found independently much earlier:
  "OFFLINE/refcount zero is not shutdown proof" - sysmon failure is
  logged but the descriptor's return value is discarded; PIL shutdown
  errors do not propagate to file close either.
- New, concrete detail this session didn't have: sysmon's real timing -
  "a 500 ms transaction wait and a 10 s acknowledgement wait" - a real
  subsys_stop() could block for 10+ seconds, not be instant.
- Critical gap for any real attempt: subsystem_put() returns void - no
  status at all. If a real stop silently failed internally, there is no
  way to know from the API itself. A real test would need an independent
  verification step afterward (e.g., this module's own ADSP-style real
  subsys_state read) rather than trusting a lack of a crash as success.
- They explicitly label their own proposed stop sequence "not executable
  yet" - confirming neither they nor this session has ever run a real
  one.

### What this adds to confidence toward the gate

Real, concrete detail now understood about what a genuine subsys_stop()
test would require (independent post-check, since the API gives no
status; tolerance for a 10+ second wait) and real, independent
confirmation that the core problem (completion isn't provable from the
API) is correctly understood, not assumed. Still have not attempted a
real teardown - this was research to know what attempting one correctly
would require, not an attempt itself.

## Checking qkx_dma_gate()'s own claim against real source, subsystem by subsystem

Re-read qkx_dma_gate() itself (module/loader.c:1014) directly rather than
from memory. It is a simple, unconditional, hard refusal for any
"Hollywood"-model device:

    pr_err("quest_kexec: Hollywood DMA shutdown/completion is
    unvalidated; refusing hardware transition\n");
    return -EOPNOTSUPP;

with an explicit comment: "No experimental flag bypasses this." This
gate does not consult qkx_backends.ko at all right now - the two are
currently separate. Bypassing or modifying this gate is a materially
bigger, more consequential decision than anything attempted this
session (everything so far has been read-only checks or symmetric
subsystem_get/put - never the real jump path), and was not attempted.

The comment's specific factual claim - "Generic shutdown does not run
their remove paths" - is checkable from source without touching
hardware, so checked it directly rather than taking it on faith:

1. Confirmed qkx_execute() (loader.c:1227) really does call
   prepare(NULL), i.e. kernel_restart_prepare(NULL), during a real jump
   - not just looked up and unused.
2. kernel_restart_prepare() (kernel/reboot.c:69) unconditionally calls
   device_shutdown() (drivers/base/core.c:3599), which walks every
   struct device and calls dev->bus->shutdown(dev) or
   dev->driver->shutdown(dev) if either is registered.
3. Checked every subsystem this session built a real backend for, for
   whether their actual platform/spi_driver registers a .shutdown field
   at all:

   | Subsystem | Driver file | .shutdown registered? |
   | --- | --- | --- |
   | Camera (cam_req_mgr) | cam_req_mgr_dev.c | No |
   | AOP/RPMh | rpmh-rsc.c | No |
   | SMMU | arm-smmu.c | No |
   | Video (Venus) | msm_v4l2_vidc.c | No |
   | SyncBoss | (spi driver) | No |
   | GPU (KGSL) | drivers/gpu/msm/*.c | No |
   | USB (dwc3) | drivers/usb/dwc3/*.c | No |
   | UFS | ufs-qcom.c | Yes - ufshcd_pltfrm_shutdown |

   ADSP/DSP (subsystem_restart-managed) is a separate case: its real
   teardown (subsys_stop()/PIL .shutdown) is driven entirely by
   subsystem_restart.c's own refcounting, never by generic
   device_shutdown() - a struct subsys_device is not torn down by this
   path at all.

4. UFS's real .shutdown (ufshcd_shutdown(), ufshcd.c:10865) does
   substantial, real work: blocks further SCSI requests
   (ufshcd_scsi_block_requests()) and waits for the doorbell to clear
   (ufshcd_wait_for_doorbell_clr()) - the exact same doorbell registers
   backend_ufs.c already independently checks. UFS is the one subsystem
   this session built a backend for that is already covered by a real,
   substantial generic kernel mechanism during a genuine kexec jump,
   independent of anything this module does.

### What this adds to confidence toward the gate

This is the first time this session has checked the gate's own stated
reasoning against source, rather than taking its comment as given. The
result is a precise, concrete confirmation, not a surprise: of the 8
non-DSP subsystems this session built real backends for, only UFS has
any generic kernel-provided shutdown coverage at all. Camera, AOP, SMMU,
Video, SyncBoss, GPU and USB would get nothing from the kernel's own
device_shutdown() during a real jump - confirming, with source-level
precision rather than general suspicion, exactly why the gate's
"unvalidated" claim is accurate for this device and why everything this
session built (and the things still missing - a safe way to drive
DSP/ADSP's real subsys_stop(), and the same for every other
subsystem_restart entity) would need to genuinely substitute for that
missing coverage before this gate's refusal could be responsibly
reconsidered. Still have not touched the gate itself, and no changes to
loader.c were made.

## Closing a gap in the previous table: DSP's underlying platform_device

The previous table noted DSP/ADSP's real teardown is subsystem_restart-
managed, separate from device_shutdown(). One loose end: subsys-pil-tz.c
assigns a .shutdown field in two places (pil_reset_ops.shutdown =
pil_shutdown_trusted at line 805, and subsys_desc.shutdown =
subsys_shutdown at line 1159) - but subsys_desc.dev = &pdev->dev means
there's also a real, underlying platform_device. Checked whether THAT
device's own platform_driver registers .shutdown (which device_shutdown()
would actually call) - confirmed it does not:

    static struct platform_driver pil_tz_driver = {
        .probe = pil_tz_driver_probe,
        .remove = pil_tz_driver_exit,
        .driver = { .name = "subsys-pil-tz", ... },
    };

No .shutdown field at all - only .probe/.remove. Both earlier .shutdown
assignments belong to pil_reset_ops and subsys_desc, PIL's own internal
vtables, only ever invoked through subsystem_restart.c's explicit
subsystem_put()-to-zero path, never through the generic device_shutdown()
walk. This closes the question with certainty rather than inference:
DSP/ADSP gets zero generic shutdown coverage during a real kexec jump,
same as the other 7 non-UFS subsystems.

## Confirming the gate is the real, live, operative barrier (not a dormant check)

Checked the second qkx_dma_gate() call site (loader.c:1457, inside the
ioctl parameter-validation path, separate from the one inside
qkx_execute() itself at line 1071 - checked twice, defense in depth).
Noticed a compile-time guard around hardware actions,
`#ifndef QKX_ENABLE_EXPERIMENTAL_JUMP` (loader.c:1436) returning
-EOPNOTSUPP for a "staging build" - worth checking whether this flag is
actually on in the project's real build, since if it were off, the
runtime gate would be moot (nothing reachable to even test).

module/Makefile:21 sets `ccflags-y += -DQKX_ENABLE_EXPERIMENTAL_JUMP`
unconditionally - this flag is always on in this project's actual build.
`execute` is reachable (qkx_execute() at loader.c:1575, same function
containing qkx_dma_gate() and prepare(NULL)). Confirms the runtime gate
studied in the last two entries is the real, live, singular, operative
barrier in this exact build - not a dormant or already-bypassed check.

## Literal confirmation of "remove paths" in the gate's own wording

Checked cam_req_mgr_dev.c's actual .remove callback (cam_req_mgr_remove,
line 777) for comparison. It is real, substantial teardown code -
cam_req_mgr_core_device_deinit(), media/video device cleanup,
mutex_destroy(&g_dev.dev_lock), resets g_dev.state - the same g_dev
struct backend_camera.c reads. But .remove() is a platform_driver field
only ever invoked on explicit driver unbind (driver_unregister(),
sysfs unbind, module exit) - never by device_shutdown(), which only
ever consults .shutdown. This is the literal, concrete match to the
gate's own wording ("does not run their remove paths"): a real,
working teardown path exists in the driver, confirmed directly, and a
real kexec jump would never reach it.

Not something this session attempted to call directly - cam_req_mgr_remove()
is static (not exported), and invoking driver-unbind-time teardown logic
on a live, possibly-in-use driver from outside is a materially more
invasive action than anything else explored this session. Purely
confirmatory research.

## What a real subsys_stop() would actually ripple into: FastRPC's SSR notifier

Checked whether anything automatically reacts to a real subsys_stop()
beyond subsystem_restart.c's own internal bookkeeping - specifically,
which drivers register an SSR notifier (subsys_notif_register_notifier)
for adsp/cdsp. Found: drivers/char/adsprpc.c (FastRPC), plus
venus_boot.c and npu_mgr.c for their own subsystems. techpack/camera
does not register one for adsp/cdsp.

Confirmed the exact chain with source, not inference:
subsystem_put()-to-zero -> subsys_stop() fires SUBSYS_BEFORE_SHUTDOWN
(subsystem_restart.c:879) -> fastrpc_restart_notifier_cb() marks that
channel's ctx->subsystemstate = SUBSYSTEM_RESTARTING -> real PIL/sysmon
shutdown runs -> subsys_stop() fires SUBSYS_AFTER_SHUTDOWN
(subsystem_restart.c:896) -> fastrpc_restart_notifier_cb() walks every
open fastrpc_file tied to that channel and calls complete(&fl->shutdown)
- waking up every blocked userspace FastRPC client on that DSP channel,
marking the channel SUBSYSTEM_DOWN.

### What this adds

This is a real, user-visible consequence beyond kernel-internal
bookkeeping: driving a genuine subsys_stop() on an actively-used DSP
channel would immediately surface as "subsystem down" to any running
application with an open FastRPC handle on it - not just a risk to this
module's own logic, a risk to whatever of Quest 2's normal running
software stack happens to be using that DSP at the time. Ties directly
into WitherBlitz's own audit finding that FastRPC's release path already
ignores several of its own results (QUEST2_OWNER_AUDIT.md) - this
confirms the notification side of that same mechanism fires for real
and would be immediately visible, not latent. Reinforces, with a
concrete causal chain now rather than general caution, why no test this
session attempted to drive any subsystem's refcount to zero.

## loader.c's existing real producer-quiescing code, and what it still doesn't cover

Read qkx_execute()'s full body between the dma_gate check and
prepare(NULL) (loader.c:1074-1227) to catalog what real, already-built
quiescing code exists there, separate from this session's own module:

- GPU: adreno_pm_suspend() (real driver suspend callback)
- USB: dwc3_gadget_suspend() (real, stops gadget traffic/Run-Stop
  without full driver unbind)
- SyncBoss: its own driver's PM suspend callback, if suspend_syncboss
  requested
- Watchdog: disabled via its own sysfs-equivalent store function
- AOP: qkx_disconnect_aop_qmp() (loader.c:956) - a real, substantial
  implementation that closes assigned QMP mailbox channels, writes a
  link-down state via raw MMIO, sends an IRQ, and waits for AOP's
  acknowledgement, if disconnect_qmp requested

Explicitly absent from this list: Camera, DSP/ADSP, Video/Venus, SMMU.
Nothing in qkx_execute() touches any of them - directly consistent
with and reinforcing exactly what qkx_dma_gate()'s comment warns about.

Checked whether qkx_disconnect_aop_qmp() incidentally covers this
session's own AOP finding (rpmh-rsc.c's tcs_in_use bitmap, zeroed blind
on probe with no hardware verification - the stale-TCS-slot race
documented in backend_aop.c). Confirmed it does not: qmp_shutdown()
lives in drivers/mailbox/msm_qmp.c, a completely separate mailbox
driver from rpmh-rsc.c. QMP is AOP's messaging/command channel; RSC/TCS
is the separate hardware command-queue AOP's firmware reads from
on wake. Even with this real, already-built AOP-QMP-disconnect code in
place, the TCS stale-slot concern remains a genuinely separate,
unaddressed gap - not already covered by existing work this session
hadn't looked at.

## Completing the qkx_execute() audit: the final jump itself, and a striking independent confirmation

Read the rest of qkx_execute() through to the actual jump
(loader.c:1292-1343). Confirms the picture from the previous entry is
complete, not partial: between the watchdog-quiesce code and the final
qkx_enter(virt_to_phys(pgd_page), virt_to_phys(code_page),
virt_to_phys(first_desc), payload_entry, payload_dtb) call, the only
remaining steps are cache maintenance (__flush_dcache_area/
__flush_icache_range) and masking interrupts/preemption
(local_irq_disable/preempt_disable). Nothing SMMU/IOMMU/DMA-related
appears anywhere between the gate check and the literal final jump -
confirmed end to end for the whole function, not just the portion
already read.

Also found something worth recording on its own: the existing rollback
path (the release_devices: label, run when an earlier step fails)
carries this comment on resuming GPU/Syncboss:

    "These callbacks can defer work or hide internal errors: return
    zero is not proof that hardware has recovered."

This is the original author independently stating, in their own
already-shipped, most load-bearing code, the exact same principle this
session's entire backend methodology has operated under all along: a
callback's success return value is evidence, not proof, of real
hardware completion. Not a new finding about the gate itself, but a
clear, independent confirmation that this session's caution (treating
"the function returned 0" as inconclusive rather than definitive, the
reasoning behind every mutex_trylock/busy-is-inconclusive and
clock-gate-first check across every backend this session built) matches
the standard the person who wrote the gate itself was already applying
to their own code, not a self-imposed extra layer of caution beyond
what the project calls for.

## AOP/TCS mitigation design: a real, partial path found via already-exported rpmh_flush()

First real attempt at designing a mitigation (not just documenting the
gap) for the stale-TCS-slot race that started this whole research
effort: rpmh-rsc.c's probe() blindly zeroes drv->tcs_in_use with no
hardware verification, safe only on a true cold boot.

Deliberately did not read RSC_DRV_CMD_ENABLE/RSC_DRV_STATUS directly
from this module - backend_aop.c already documented why: AOP is
"always-on, security- and power-sequencing-critical infrastructure
where a third wrong guess is least acceptable to risk," after two real
crashes this session from the same category of mistake elsewhere.
Looked instead for an existing, driver-sanctioned, already-exported
mechanism.

Traced rpmh_flush() (rpmh.c:552, EXPORT_SYMBOL, already called once in
loader.c for one specific regulator client) end to end:

- It calls rpmh_rsc_invalidate(drv) first, retrying on -EAGAIN.
- rpmh_rsc_invalidate() calls tcs_invalidate() for SLEEP_TCS and
  WAKE_TCS only (rpmh-rsc.c:154) - explicitly, deliberately not
  ACTIVE_TCS.
- tcs_invalidate() (rpmh-rsc.c:123) is itself hardware-state-aware, not
  blind: for each bank of that type, it calls tcs_is_free(drv, m) -
  literally !test_bit(m, drv->tcs_in_use), confirming tcs_in_use (the
  exact field backend_aop.c already reads informationally) is the real
  authoritative per-bank record. If any bank isn't free it returns
  -EAGAIN (a safe refusal, never a forced override) rather than
  proceeding. Only once every bank of that type is confirmed free does
  it write real hardware registers (RSC_DRV_CMD_ENABLE=0,
  RSC_DRV_CMD_WAIT_FOR_CMPL=0) via the driver's own
  write_tcs_reg_sync(), then zero the software tcs->slots bitmap to
  match.
- flush_batch() (rpmh.c:355), rpmh_flush()'s other half, is explicitly
  scoped the same way ("Send Sleep/Wake requests to the controller" -
  its own comment) via rpmh_rsc_write_ctrl_data(), never an active-state
  send.

get_rpmh_ctrlr(dev) (rpmh.c:72) resolves dev->parent's drvdata to find
the rsc_drv - meaning rpmh_flush() needs a CHILD/consumer device of the
RSC in question, not the RSC's own platform device. loader.c's existing
call uses one specific regulator client under "18200000.rsc"
(APPS_RSC); the same pattern would need a real child device under
"af20000.rsc" (DISP_RSC) too for full coverage.

### What this is, and what it honestly is not

This is a real, concrete, safe path - calling rpmh_flush() for both
RSCs right before a jump would genuinely invalidate SLEEP_TCS/WAKE_TCS
banks using the driver's own hardware-aware, never-forcing logic,
narrowing the stale-slot concern for those two TCS types from
"completely unaddressed" to "addressable via an already-exported,
already-used-once mechanism."

It does not touch ACTIVE_TCS at all - confirmed, not assumed, by
reading every function rpmh_flush() calls. The one function that even
checks active-TCS idle state, rpmh_rsc_ctrlr_is_idle() (rpmh-rsc.c:593),
is not EXPORT_SYMBOL'd - rpmh-rsc.c has zero exported symbols of its
own; rpmh_rsc_invalidate() and rpmh_rsc_ctrlr_is_idle() are only
reachable because rpmh-rsc.o and rpmh.o are compiled into the same
module (qcom_rpmh.ko, drivers/soc/qcom/Makefile:14-16) - genuinely
unreachable from an external module like this one without the kernel
itself exporting a new symbol. There is no safe way found yet to even
verify, let alone clear, active-TCS state from outside the driver.

This is real forward progress on the single most important open
question from this session, not a complete answer: the gap is now
precisely bounded (sleep/wake addressable, active genuinely not) rather
than uniformly unknown. Nothing in this entry was executed on real
hardware - this is design/source research only.

## Precisely bounding the active-TCS gap: it's specific to APPS_RSC, not both RSCs

Checked whether the active-TCS gap (identified in the previous entry -
rpmh_flush() never touches ACTIVE_TCS, and the only idle-check function
for it isn't exported) is even real on this specific hardware. The
driver's own comment notes some RSCs have no dedicated ACTIVE_TCS bank
at all, in which case active votes are redirected to WAKE_TCS instead
(rpmh-rsc.c:185-193) - which rpmh_flush() does cover. Whether that
applies here is a config fact, not something to guess at - read it
directly from the live device's own device tree
(/proc/device-tree/soc/rsc@18200000/qcom,tcs-config and
.../rsc@af20000/qcom,tcs-config), pure read-only, zero hardware risk -
not a live register, just exposing already-static DT configuration data.

Parsed the raw (type, num_tcs) pairs using the real constants from
include/dt-bindings/soc/qcom,rpmh-rsc.h (SLEEP_TCS=0, WAKE_TCS=1,
ACTIVE_TCS=2, CONTROL_TCS=3):

| RSC | SLEEP_TCS | WAKE_TCS | ACTIVE_TCS | CONTROL_TCS |
| --- | --- | --- | --- | --- |
| APPS_RSC (18200000.rsc) | 3 | 3 | 2 | 1 |
| DISP_RSC (af20000.rsc) | 1 | 1 | 0 | 0 |

DISP_RSC has zero dedicated ACTIVE_TCS banks - its active votes are
redirected to WAKE_TCS per the driver's own repurposing logic, which
rpmh_flush()'s WAKE_TCS invalidation already covers. The active-TCS gap
does not exist at all for DISP_RSC.

APPS_RSC genuinely has 2 dedicated ACTIVE_TCS banks - the gap is real
and specific to this RSC only.

### What this adds

Narrows the single remaining open piece of the AOP/TCS problem from "an
unbounded ACTIVE_TCS concern across AOP" to a precisely scoped one:
specifically APPS_RSC's 2 active-TCS banks, with DISP_RSC's side of the
problem already fully addressable by the rpmh_flush()-based mitigation
designed in the previous entry. This is real narrowing of scope via a
simple, safe, zero-risk device-tree read - not a new mitigation for the
remaining gap, but a precise statement of exactly how small it now is.

## Turning the active-TCS bound into working code: backend_aop.c now reports it precisely

The previous entry bounded the active-TCS gap to APPS_RSC's 2 dedicated
banks via a live DT read. Worked out exactly which bits those are in
tcs_in_use by reading rpmh_probe_tcs_config()'s own offset-assignment
loop (rpmh-rsc.c:803-823): it walks tcs_cfg[] in DT property order,
skips entries with num_tcs==0 or type==CONTROL_TCS, and assigns
tcs->offset as a running total (st) over the remaining types in that
same order. For APPS_RSC's actual stored order - (ACTIVE_TCS,2),
(SLEEP_TCS,3), (WAKE_TCS,3), (CONTROL_TCS,1) - this gives ACTIVE_TCS
offset=0 (bits 0-1), SLEEP_TCS offset=2, WAKE_TCS offset=5,
CONTROL_TCS untracked. Derived, not guessed, directly from this
device's own live /proc/device-tree data and the real probe() code.

Extended qkx_aop_log_tcs_bitmap() (backend_aop.c) to accept an
active_tcs_mask parameter and report whether those specific bits are
set, alongside the existing raw tcs_in_use log line - no new risk, same
already-proven-safe ordinary-memory read, just a more precise
interpretation of data already being read. APPS_RSC passes 0x3;
DISP_RSC passes 0 (confirmed via the same DT read to have no dedicated
ACTIVE_TCS bank, so there's nothing to isolate there).

Verified live: clean insmod/rmmod, zero BUG/WARNING/Oops. Real output:

    [aop] 18200000.rsc tcs_in_use=0x0 (this kernel's own view only)
    [aop] 18200000.rsc active_tcs_busy=0 (mask=0x3 - the one gap
    rpmh_flush() doesn't cover)

Confirms, with the precision this research built up over several
entries, that the one specific remaining gap in the AOP/TCS problem -
APPS_RSC's 2 active-TCS banks - is free right now. Same honest caveat
as every other reading of tcs_in_use still applies: this is the
CURRENT kernel's own view, not proof about what a future kernel after
a jump would see - but it is now the most precisely-targeted piece of
evidence this research has produced about the single hardest part of
the original finding.
