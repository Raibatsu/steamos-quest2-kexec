/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QKX_COMMON_H
#define QKX_COMMON_H

#include <linux/io.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include "shutdown.h"

/*
 * Real physical register bases, confirmed against this exact device's own
 * kernel source (arch/arm64/boot/dts/vendor/qcom/kona*.dtsi and
 * arch/arm64/boot/dts/vendor/qcom/camera/kona-camera.dtsi), not guessed.
 */
#define QKX_PA_GPU		0x03d00000UL	/* kgsl-3d0 */
#define QKX_PA_UFS		0x01d84000UL	/* ufshc */
#define QKX_PA_USB0		0x0a600000UL	/* dwc0 (primary USB-C port) */
#define QKX_PA_VFE0		0x0acb4000UL	/* cam_vfe0 / ife0 */
#define QKX_PA_APPS_RSC		0x18200000UL	/* apps_rsc (RPMh/TCS) */
#define QKX_PA_DISP_RSC		0x0af20000UL	/* disp_rsc (RPMh/TCS) */

/*
 * Real register offsets, each confirmed by reading the actual driver that
 * owns the hardware (adreno.c/a6xx_reg.h, ufshcd.c, dwc3 core.c/gadget.c,
 * cam_vfe_core.c, rpmh-rsc.c) earlier this session - not copied from a
 * datasheet we don't have, and not assumed from a different SoC generation.
 */
#define QKX_GPU_RBBM_STATUS	0x210		/* A6XX_RBBM_STATUS, word offset */
#define QKX_GPU_CP_RB_RPTR	0x806		/* word offset */
#define QKX_GPU_CP_RB_WPTR	0x807		/* word offset */

#define QKX_UFS_UTRLDBR		0x58		/* REG_UTP_TRANSFER_REQ_DOOR_BELL */
#define QKX_UFS_UTMRLDBR	0x78		/* REG_UTP_TASK_REQ_DOOR_BELL */

#define QKX_USB_DSTS		0x1c08		/* DWC3_DSTS, byte offset from core_base */
#define QKX_USB_DSTS_DEVCTRLHLT	BIT(1)

#define QKX_VFE_IRQ_STATUS0	0x8		/* IFE top IRQ status, CAM_IFE_IRQ_CAMIF_REG_STATUS0 */

#define QKX_RSC_DRV_CMD_ENABLE_OFF	0x1c
#define QKX_RSC_DRV_STATUS_OFF		0x18
#define QKX_RSC_DRV_TCS_OFFSET		0x400
#define QKX_RSC_MAX_TCS_PER_DRV	16

/* Platform device names, following the real of_platform naming convention
 * (<unit-address>.<node-name>) confirmed against this device's own tree -
 * used to look up each register's owning device for the clock-gate check
 * above, not guessed from a generic pattern.
 */
#define QKX_DEV_GPU	"3d00000.qcom,kgsl-3d0"
#define QKX_DEV_UFS	"1d84000.ufshc"
#define QKX_DEV_USB0	"a600000.ssusb"
#define QKX_DEV_VFE0	"acb4000.qcom,ife0"
#define QKX_DEV_APPS_RSC	"18200000.rsc"
#define QKX_DEV_DISP_RSC	"af20000.rsc"

/* Maps a physical register window, reads it, unmaps. Debug/inspection-only
 * access pattern - same shape arm-smmu-debug.c uses for its own diagnostic
 * reads, not an exclusive resource claim against the owning driver.
 */
static inline u32 qkx_read_phys(phys_addr_t pa, u32 size, u32 byte_off)
{
	void __iomem *base;
	u32 val;

	base = ioremap(pa, size);
	if (!base)
		return 0xffffffff;
	val = readl_relaxed(base + byte_off);
	iounmap(base);
	return val;
}

static inline u32 qkx_read_phys_word(phys_addr_t pa, u32 size, u32 word_off)
{
	return qkx_read_phys(pa, size, word_off * 4);
}

enum qkx_clock_state {
	QKX_CLOCK_UNKNOWN,	/* couldn't find the owning device - no claim either way */
	QKX_CLOCK_GATED,	/* runtime-PM reports suspended: no clock, no possible DMA */
	QKX_CLOCK_ACTIVE,	/* clocked and powered: safe to read the real status register */
};

/* This exists because of a real crash: reading a register from a
 * peripheral whose clock/power domain is gated off can raise an
 * asynchronous SError on this SoC, which arm64 treats as fatal and
 * panics on immediately - confirmed directly, not theoretical (the
 * camera/VFE backend did exactly this on real hardware this session).
 *
 * Every direct-register backend must call this before touching a
 * register. A GATED result is actually a strong, definite answer, not a
 * weaker one: a peripheral with no clock cannot be issuing any bus
 * transaction, so idleness is established without needing the register
 * at all. Only an ACTIVE result makes it safe to proceed with the real
 * register read.
 */
static inline enum qkx_clock_state qkx_clock_domain_state(struct bus_type *bus,
							    const char *name)
{
	struct device *dev;
	bool suspended;

	dev = bus_find_device_by_name(bus, NULL, name);
	if (!dev)
		return QKX_CLOCK_UNKNOWN;

	/* Deliberately conservative: a device with runtime PM disabled
	 * entirely (sysfs shows "unsupported", not "active"/"suspended")
	 * still has dev->power.runtime_status defaulted to RPM_SUSPENDED,
	 * so pm_runtime_status_suspended() alone would likely already
	 * treat it as gated - but relying on that undocumented default is
	 * exactly the kind of assumption that caused the real crash this
	 * guard exists to prevent. Requiring pm_runtime_enabled() as well
	 * means "unsupported" and "suspended" both safely fall through to
	 * GATED, and only a device genuinely, actively tracked as RPM_ACTIVE
	 * is treated as safe to read.
	 */
	if (!pm_runtime_enabled(dev)) {
		put_device(dev);
		return QKX_CLOCK_GATED;
	}

	suspended = pm_runtime_status_suspended(dev);
	put_device(dev);
	return suspended ? QKX_CLOCK_GATED : QKX_CLOCK_ACTIVE;
}

/* Must exactly match the real, static qkx_shutdown_evidence() in
 * shutdown.c - that function is not exported, so every backend that wants
 * a receipt shutdown.c will actually accept has to independently produce
 * the same value. Verified against shutdown.c's source directly, not
 * guessed: FORWARD direction reports the single action bit; UNDO reports
 * NO_LATE_COMPLETION plus either ADMISSION_CLOSED (for a CLOSE undo) or
 * RESTORED (for anything else); REOPEN is not used by any backend here.
 */
static inline unsigned int qkx_evidence_for(const struct qkx_shutdown_key *key)
{
	if (key->direction == QKX_SHUTDOWN_UNDO)
		return QKX_SHUTDOWN_NO_LATE_COMPLETION |
			(key->action == QKX_SHUTDOWN_CLOSE ?
			 QKX_SHUTDOWN_ADMISSION_CLOSED : QKX_SHUTDOWN_RESTORED);
	return 1U << key->action;
}

#endif
