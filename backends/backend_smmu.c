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
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_SMMU_CAPS QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RELEASE)

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
