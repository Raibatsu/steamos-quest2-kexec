/* SPDX-License-Identifier: GPL-2.0-only
 *
 * UFS shutdown-safety backend.
 *
 * REVISED after this exact backend caused a real crash on real hardware:
 * reading REG_UTP_TRANSFER_REQ_DOOR_BELL / REG_UTP_TASK_REQ_DOOR_BELL
 * raised a fatal asynchronous SError even though qkx_clock_domain_state()
 * reported this device genuinely runtime-PM active. Root cause: UFS has
 * its own autonomous link-hibernate state (the "auto_hibern8" feature -
 * confirmed present via this device's own ufs-sysfs attributes), which
 * operates entirely independently of Linux's generic PM model. Being
 * "active" in the generic sense does not mean the UniPro link itself
 * isn't hibernating underneath it.
 *
 * This backend no longer reads any raw register. A GATED clock is still
 * a confident "idle" answer (no clock, no possible transaction); an
 * ACTIVE result is honestly reported as PENDING rather than risking
 * another crash to confirm it.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_UFS_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_ufs_doorbells_clear(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_UFS);

	if (clk == QKX_CLOCK_GATED)
		return true;
	return false; /* ACTIVE or UNKNOWN: not confirmed, no register read attempted */
}

static int ufs_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_UFS_CAPS;
	inv->rollback_capabilities = QKX_UFS_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int ufs_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_ufs_doorbells_clear()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int ufs_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return ufs_begin(context, key, receipt);
}

static int ufs_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_ufs = {
	.subsystem = QKX_SHUTDOWN_UFS,
	.inspect = ufs_inspect,
	.begin = ufs_begin,
	.poll = ufs_poll,
	.rollback_begin = ufs_rollback,
	.rollback_poll = ufs_rollback,
};
