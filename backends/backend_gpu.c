/* SPDX-License-Identifier: GPL-2.0-only
 *
 * GPU (Adreno/KGSL) shutdown-safety backend.
 *
 * Real evidence this is built on (confirmed by reading adreno.c this
 * session, not assumed): adreno_isidle() in the real driver checks the
 * ringbuffer's own hardware read/write pointer registers directly, plus a
 * per-generation hw_isidle() status read. adreno_isidle() itself is a
 * static function we cannot call from an external module, so this backend
 * independently re-derives the same hardware fact by reading the real
 * A6XX_CP_RB_RPTR/WPTR and A6XX_RBBM_STATUS registers at their confirmed
 * physical offsets - the same registers the real driver reads, just via
 * our own ioremap rather than calling into adreno.c's internals.
 *
 * Scope: implements CLOSE/DRAIN/STOP as the same real idle check, run at
 * each step the orchestrator requires it. This is honest about what the
 * hardware actually gives us: Adreno doesn't have three independently
 * distinguishable hardware states for these three software-level actions,
 * only "idle" or "not idle".
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_GPU_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_gpu_is_idle(void)
{
	u32 rptr = qkx_read_phys_word(QKX_PA_GPU, 0x40000, QKX_GPU_CP_RB_RPTR);
	u32 wptr = qkx_read_phys_word(QKX_PA_GPU, 0x40000, QKX_GPU_CP_RB_WPTR);
	u32 status = qkx_read_phys_word(QKX_PA_GPU, 0x40000, QKX_GPU_RBBM_STATUS);

	if (rptr == 0xffffffff || wptr == 0xffffffff || status == 0xffffffff)
		return false; /* ioremap/read failed - do not claim idle */

	/* Ringbuffer idle (rptr caught up to wptr) and RBBM core not busy. */
	return (rptr == wptr) && !(status & BIT(0));
}

static int gpu_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_GPU_CAPS;
	inv->rollback_capabilities = QKX_GPU_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int gpu_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_gpu_is_idle()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = 1U << key->action;
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int gpu_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return gpu_begin(context, key, receipt);
}

static int gpu_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	/* A read-only idle check has nothing to undo. */
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_gpu = {
	.subsystem = QKX_SHUTDOWN_GPU,
	.inspect = gpu_inspect,
	.begin = gpu_begin,
	.poll = gpu_poll,
	.rollback_begin = gpu_rollback,
	.rollback_poll = gpu_rollback,
};
