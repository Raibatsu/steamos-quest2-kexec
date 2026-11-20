/* SPDX-License-Identifier: GPL-2.0-only
 *
 * GPU (Adreno/KGSL) shutdown-safety backend.
 *
 * REVISED after two real crashes on real hardware this session (camera's
 * VFE register read, then UFS's doorbell register read, both raising a
 * fatal asynchronous SError). The second crash specifically disproved the
 * assumption behind the first fix: UFS was genuinely runtime-PM "active"
 * and still faulted, because UFS has its own autonomous link-hibernate
 * state (auto_hibern8) entirely separate from Linux's generic PM model.
 * There is no reason to assume Adreno/KGSL lacks an equivalent autonomous
 * low-power sub-state this module cannot see from outside the driver.
 *
 * Given that, this backend no longer performs any raw MMIO register read
 * at all, even when the clock-gate check reports the device active. It
 * relies solely on qkx_clock_domain_state(): a GATED result is still a
 * strong, confident "idle" answer (no clock, no possible transaction).
 * An ACTIVE result is no longer treated as "safe to go read a register" -
 * it is honestly reported as PENDING (not confirmed), because confirming
 * it would require exactly the kind of raw read that has now caused two
 * real reboots. This trades real hardware-register-strength evidence for
 * guaranteed safety against a demonstrated, repeating crash class.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_GPU_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_gpu_is_idle(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_GPU);

	if (clk == QKX_CLOCK_GATED)
		return true;
	return false; /* ACTIVE or UNKNOWN: not confirmed, no register read attempted */
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
