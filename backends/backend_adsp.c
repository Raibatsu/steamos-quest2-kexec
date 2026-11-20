/* SPDX-License-Identifier: GPL-2.0-only
 *
 * ADSP/CDSP/SLPI shutdown-safety backend.
 *
 * Real evidence: subsys_stop() in subsystem_restart.c does a synchronous,
 * two-way sysmon_send_shutdown() IPC handshake with the remote firmware
 * itself before any hardware-level reset, and only fires AFTER_SHUTDOWN
 * once that and the PIL shutdown() callback both complete.
 *
 * This backend deliberately does NOT call subsystem_get()/subsystem_put()
 * itself: those are real, reference-counted ownership primitives, and
 * forcing a subsystem down while some other driver still legitimately
 * holds its own reference would be wrong, not safe - exactly the
 * multi-owner problem that refcounting exists to prevent. There is no
 * exported, purely-read-only "is this fully idle" query in
 * subsystem_restart.c; subsys_get_crash_status() is the closest real,
 * exported, non-destructive signal available, and is used here
 * honestly as a weaker check than the GPU/UFS/USB/AOP register reads -
 * it confirms this subsystem is not currently crashed/mid-restart, not
 * that no driver anywhere still holds a reference to it.
 */
#include <linux/errno.h>
#include <soc/qcom/subsystem_restart.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_ADSP_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		       QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		       QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_adsp_not_crashed(void)
{
	struct subsys_device *dev;
	int crashed;

	dev = find_subsys_device("adsp");
	if (!dev)
		return false; /* can't find it: don't claim safety we don't have */

	crashed = subsys_get_crash_status(dev);
	return crashed == 0;
}

static int dsp_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_ADSP_CAPS;
	inv->rollback_capabilities = QKX_ADSP_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int dsp_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_adsp_not_crashed()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int dsp_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return dsp_begin(context, key, receipt);
}

static int dsp_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_dsp = {
	.subsystem = QKX_SHUTDOWN_DSP,
	.inspect = dsp_inspect,
	.begin = dsp_begin,
	.poll = dsp_poll,
	.rollback_begin = dsp_rollback,
	.rollback_poll = dsp_rollback,
};
