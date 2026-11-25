# Booting the Quest 2 into SteamOS (and shutting it down)

State saved on 2026-11-25. Everything the headset needs is on this computer; nothing on the headset has to be restored.

## What lives where
| What | Where |
| :--- | :--- |
| SteamOS root filesystem (all our rootfs changes: services, binaries, Wi-Fi/BT firmware, camera tools) | `/mnt/HDD/q2steamos/disk.img` (served over NBD; never edit it while `nbd_server.py` runs) |
| Boot bundle: kernel `Image-linux72`, stage 1 `initramfs-linux72.cpio.gz`, `quest_kexec.ko`, `q2kexec`, `dtb/boot-{100,130,140,160,180}-fan.dtb`, `kxc.sh`, `host/` scripts, `MANIFEST.sha256` | `/mnt/HDD/quest2-boot/` (not in git: kernel, DTBs, module) |
| Boot script | `linux72/tools/boot_quest2.sh` (copy in the bundle) |
| Kernel tree (7.2 + upstream layer + our kexec changes), build output | `/mnt/HDD/quest2-linux72/linux-up`, `/mnt/HDD/quest2-linux72/out-up` |
| Stock Meta files extracted read-only (never commit) | `/mnt/HDD/quest2-linux72/meta-extract/files/` |

## Boot, normal case
1. Power the headset on and let Horizon boot. Trigger Singularity root as usual. Wake the headset (put it on, or cover the proximity sensor).
2. `/mnt/HDD/quest2-boot/boot_quest2.sh` - verifies the bundle, checks root and that the display is awake, stops the DSP/tracking services, pushes the files, and does a **dry run** of the
   memory claim (nothing jumps). Look at the claimed ranges it prints.
3. `/mnt/HDD/quest2-boot/boot_quest2.sh --go` - starts `nbd_server.py` / `kmsg_udp_recv.py` (bound to 10.42.0.1), does the jump, attaches the NBD root in Stage 1, switches to Stage 2, waits ~1 minute and prints
   the state of `q2-headtrack`, `q2-camd`, `q2-ctltrack`, `q2-radios`, Wi-Fi and Bluetooth.
4. Shell: `telnet 10.42.0.2 2323`. Start SteamVR with `q2vr start` (give it about a minute). Cameras, controller solver and radios start by themselves.

The script has **not been run end to end as a script**: each step mirrors what was done by hand. It stops before the jump if any check fails. If root on your Singularity build is only reachable over the local socket
(`nc 127.0.0.1 1234`), run the Horizon-side steps by hand (HANDOFF_STAGE5.md), then `boot_quest2.sh` from step 6 on is the same as below.

## Manual fallback (what the script does)
Horizon, as root: wake the display (`am broadcast -a com.oculus.vrpowermanager.prox_close`, check `dumpsys power` shows `mWakefulness=Awake` and `mScreenState=ON`), `stop` the `adsprpcd`, `cdsprpcd`, `trackingservice`,
`trackingfidelityservice` services, `echo 3 > /proc/sys/vm/drop_caches; echo 1 > /proc/sys/vm/compact_memory`. `adb push` the bundle files into `/data/local/tmp` (the initramfs must be named `initramfs-probe.cpio.gz`), `sync`,
then `sh /data/local/tmp/kxc.sh 4608 dry fan`, then `sh /data/local/tmp/kxc.sh 4608`. In Stage 1 (`telnet 10.42.0.2`): `busybox nbd-client 10.42.0.1 10809 /dev/nbd0`, `mount -t btrfs -o rw /dev/nbd0 /mnt/newroot`, write
`/run/stage2.sh` (four `mount --move` for proc/sys/dev/run, then `exec switch_root /mnt/newroot /sbin/init`) and make it executable; Stage 2 answers on port 2323.

## Rebuilding the bundle (after changing the kernel or the DTS)
Build in `/mnt/HDD/quest2-linux72/linux-up` (`make O=../out-up ... Image qcom/sm8250-oculus-hollywood-kexec.dtb`), copy `Image` and the DTB into the bundle, then
`linux72/tools/make_claim_dtbs.sh <dtb> <Image> <initramfs> /mnt/HDD/quest2-boot/dtb fan` (it bakes in the initramfs address, so rerun it whenever the Image or initramfs changes) and regenerate `MANIFEST.sha256`.
For a kernel-only iteration without Horizon: Linux to Linux kexec from Stage 2 (`make_q2kexec_dtb.py` with a fresh `/sys/firmware/fdt`, then `q2kexec -B <base> -D <dtb> Image initrd`), see `NOTES.md`.

## Shutting down (lesson from 2026-11-25)
In Stage 2: `q2vr stop`, wait, `systemctl stop q2-ctltrack q2-camd` (**stops AND deinitializes the cameras**; killing them abruptly leaves the MCU "initialized"), `sync`, then `systemctl poweroff`.
The headset drops off the network after ~18 s and then shows up on USB as the stock Quest 2 (`2833:0083`, bcdDevice 4.19) with no adb: that is its powered-off/charging state, not a running Horizon. Unplug USB or hold
the power button to be certain it is fully off. All Linux-side state is RAM-only and gone after that; the rootfs on this computer is unaffected. Then stop the host daemons (`pkill -f nbd_server.py; pkill -f kmsg_udp_recv.py`).

## Not in the bundle on purpose
Singularity itself (re-trigger after every Horizon boot), Wi-Fi firmware (already inside the rootfs under `/lib/firmware/ath11k/QCA6390/hw2.0/`; source: this computer's `linux-firmware`).
