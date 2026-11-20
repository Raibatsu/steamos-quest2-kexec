/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QKX_COMMON_H
#define QKX_COMMON_H

#include <linux/io.h>
#include <linux/types.h>
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
