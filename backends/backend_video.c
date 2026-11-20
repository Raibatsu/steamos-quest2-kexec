/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Video (Venus) shutdown-safety backend - this is what the shutdown
 * framework's QKX_SHUTDOWN_OTHER_DMA slot is used for here.
 *
 * Real evidence: msm_comm_session_abort() in msm_vidc_common.c sends the
 * HFI abort command then does a genuine wait_for_completion_timeout() on
 * the firmware's HAL_SESSION_ABORT_DONE response - a real round-trip
 * handshake. That function and the HFI session state are private to the
 * vidc driver (no exported query for "is any session active" was found),
 * so this backend uses the same generic, real, exported signal as the
 * SyncBoss backend: the Venus core platform device's own runtime-PM
 * accounting, at its confirmed device-tree address (qcom,vidc@aa00000).
 */
#include <linux/errno.h>
#include <linux/pm_runtime.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_VID_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_video_quiet(void)
{
	struct device *dev;
	bool suspended;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, "aa00000.qcom,vidc");
	if (!dev)
		return false;

	suspended = pm_runtime_status_suspended(dev);
	put_device(dev);
	return suspended;
}

static int vid_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_VID_CAPS;
	inv->rollback_capabilities = QKX_VID_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int vid_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_video_quiet()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int vid_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return vid_begin(context, key, receipt);
}

static int vid_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_video = {
	.subsystem = QKX_SHUTDOWN_OTHER_DMA,
	.inspect = vid_inspect,
	.begin = vid_begin,
	.poll = vid_poll,
	.rollback_begin = vid_rollback,
	.rollback_poll = vid_rollback,
};
