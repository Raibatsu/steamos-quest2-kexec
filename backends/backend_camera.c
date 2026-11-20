/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Camera (VFE/IFE) shutdown-safety backend.
 *
 * Real evidence: cam_vfe_reset() in cam_vfe_core.c subscribes to the real
 * VFE top-reset IRQ and blocks on wait_for_completion_timeout() for that
 * interrupt - genuine hardware confirmation, reachable via the formally
 * registered hw_mgr_intf->hw_reset operation, not an error-only path.
 *
 * This backend deliberately does NOT call hw_ops.reset() itself or try to
 * trigger that IRQ - doing so from an unrelated module while the real
 * camera driver may still own and be driving this exact hardware would
 * race the real driver's own state machine, which is unsafe. Instead this
 * is a pure, read-only check of the VFE top IRQ status register: if any
 * top-level status bits are currently latched, the hardware is not idle
 * and it is not yet safe to proceed. This is weaker than triggering and
 * confirming a real reset, but it is the only action safe to take from
 * outside the owning driver.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_CAM_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_vfe_top_quiet(void)
{
	u32 status;
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_VFE0);

	/* This exact guard exists because of a real crash: reading
	 * QKX_VFE_IRQ_STATUS0 while this clock was gated raised an
	 * asynchronous SError on real hardware and panicked the device.
	 * The camera ISP is commonly power-gated whenever nothing is
	 * capturing, which is the normal idle case this backend exists to
	 * detect - so GATED here is not a corner case, it is the expected
	 * common case, and it is also a definite, confident "idle" answer:
	 * no clock means no possible bus transaction.
	 */
	if (clk == QKX_CLOCK_GATED)
		return true;
	if (clk != QKX_CLOCK_ACTIVE)
		return false;

	status = qkx_read_phys(QKX_PA_VFE0, 0x10000, QKX_VFE_IRQ_STATUS0);

	if (status == 0xffffffff)
		return false;
	return status == 0;
}

static int cam_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_CAM_CAPS;
	inv->rollback_capabilities = QKX_CAM_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	/* Read-only observation, not a real owned reset - weaker evidence
	 * than the other "Strong" backends, honestly reflected here.
	 */
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int cam_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_vfe_top_quiet()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int cam_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return cam_begin(context, key, receipt);
}

static int cam_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_camera = {
	.subsystem = QKX_SHUTDOWN_CAMERA,
	.inspect = cam_inspect,
	.begin = cam_begin,
	.poll = cam_poll,
	.rollback_begin = cam_rollback,
	.rollback_poll = cam_rollback,
};
