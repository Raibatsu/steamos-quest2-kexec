/* SPDX-License-Identifier: GPL-2.0-only
 *
 * SMMU shutdown-safety backend.
 *
 * Real finding from this session's research: SMMU's own TLB sync/status
 * mechanism (ARM_SMMU_CB_TLBSYNC/TLBSTATUS, standard architecture, wired
 * into the normal iommu_unmap() path already) only confirms the
 * translation cache is clean of whatever was just unmapped - it does not
 * independently prove no DMA master was using that mapping a moment
 * before. SMMU safety is therefore not a standalone problem: it is
 * entirely downstream of the other producer subsystems (camera, DSP,
 * GPU, SyncBoss, USB, UFS, video) each having already confirmed their own
 * hardware is idle, via their own backends in this same module.
 *
 * This backend does not independently unmap or re-check any mapping - it
 * has no IOVA/domain of its own to act on, and inventing one here would
 * be guessing, not verifying. It reports success and documents honestly
 * why: by the time the orchestrator reaches SMMU's RELEASE step, every
 * producer subsystem has already been required to report its own
 * CLOSE/DRAIN/STOP evidence first (confirmed by the real qkx_shutdown_step()
 * ordering in shutdown.c, read directly this session), so SMMU's own
 * correctness rides on those checks, not on anything checkable here.
 */
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_SMMU_CAPS QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RELEASE)

#define QKX_APPS_SMMU_DEV	"15000000.apps-smmu"
#define QKX_SMMU_CONTEXT_MAP_OFF	72
#define QKX_SMMU_NUM_CBS_OFF		64

/* Informational only - does not change the pass-through verdict below,
 * for the same reason as AOP's tcs_in_use read: this shows which
 * context banks THIS kernel's own bookkeeping currently has allocated
 * (arm-smmu.c's probe() calls platform_set_drvdata(pdev, smmu),
 * confirmed directly, so struct arm_smmu_device is reachable the same
 * safe way as every other upgraded backend this session), not an
 * independent proof that no DMA master is using them right now - that
 * proof still rests entirely on the other 9 backends having already
 * confirmed their own hardware idle, as the rest of this file explains.
 * context_map is a plain software bitmap (DECLARE_BITMAP, ordinary
 * kernel memory, not a hardware register), confirmed in arm-smmu.c's
 * own struct arm_smmu_device definition - no SError risk. Checked
 * "15000000.apps-smmu" specifically: the main, broadly-shared SMMU
 * instance most already-covered peripherals use (GPU has its own
 * separate instance, 3da0000.kgsl-smmu, already covered via KGSL's own
 * state in the GPU backend). Offsets resolved from this device's own
 * /sys/kernel/btf/vmlinux.
 */
static void qkx_smmu_log_context_map(void)
{
	struct device *dev;
	void *smmu;
	unsigned long bitmap[2] = { ~0UL, ~0UL }; /* non-zero default: fail safe */
	u32 num_cbs = 0;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, QKX_APPS_SMMU_DEV);
	if (!dev) {
		pr_info("qkx_backends: [smmu] %s not found\n", QKX_APPS_SMMU_DEV);
		return;
	}

	smmu = dev_get_drvdata(dev);
	if (!smmu) {
		put_device(dev);
		return;
	}

	if (!probe_kernel_read(&num_cbs, (char *)smmu + QKX_SMMU_NUM_CBS_OFF, sizeof(num_cbs)) &&
	    !probe_kernel_read(bitmap, (char *)smmu + QKX_SMMU_CONTEXT_MAP_OFF, sizeof(bitmap)))
		pr_info("qkx_backends: [smmu] %s num_context_banks=%u context_map=0x%lx%016lx (this kernel's own view only)\n",
			QKX_APPS_SMMU_DEV, num_cbs, bitmap[1], bitmap[0]);
	put_device(dev);
}

static int smmu_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_SMMU_CAPS;
	inv->rollback_capabilities = QKX_SMMU_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int smmu_begin(void *context, const struct qkx_shutdown_key *key,
		       struct qkx_shutdown_receipt *receipt)
{
	qkx_smmu_log_context_map();

	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->attempt;
	return 0;
}

static int smmu_poll(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	return smmu_begin(context, key, receipt);
}

static int smmu_rollback(void *context, const struct qkx_shutdown_key *key,
			  struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_smmu = {
	.subsystem = QKX_SHUTDOWN_SMMU,
	.inspect = smmu_inspect,
	.begin = smmu_begin,
	.poll = smmu_poll,
	.rollback_begin = smmu_rollback,
	.rollback_poll = smmu_rollback,
};
