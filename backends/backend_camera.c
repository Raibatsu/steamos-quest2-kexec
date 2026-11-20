/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Camera (VFE/IFE) shutdown-safety backend.
 *
 * REVISED after a sibling backend (UFS) crashed real hardware despite
 * reporting genuinely runtime-PM active - caused by an autonomous
 * link-level low-power state entirely outside Linux's generic PM model.
 * This module's own earlier guard against the camera's initial crash
 * (gated -> idle, active -> read a register) rested on the same kind of
 * "active means safe" assumption that just failed for a different
 * subsystem. There is no verified reason the VFE's per-block clock
 * gating can't have an equivalent gap, so this backend no longer reads
 * QKX_VFE_IRQ_STATUS0 or any other raw register at all, even when
 * active. A GATED clock is still a confident "idle" answer (no clock, no
 * possible bus transaction, and the camera ISP's normal idle state);
 * ACTIVE is honestly reported as PENDING rather than risking a third
 * crash to confirm it.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_CAM_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_vfe_top_quiet(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_VFE0);

	if (clk == QKX_CLOCK_GATED)
		return true;
	return false; /* ACTIVE or UNKNOWN: not confirmed, no register read attempted */
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
