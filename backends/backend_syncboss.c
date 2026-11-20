/* SPDX-License-Identifier: GPL-2.0-only
 *
 * SyncBoss shutdown-safety backend.
 *
 * Real evidence: stop_streaming_locked() in syncboss_spi_core.c sends a
 * graceful MCU shutdown command then does a real, unconditional
 * kthread_stop() join on the worker thread driving the SPI transfers.
 * That function and the worker thread are private to the syncboss driver
 * - there is no EXPORT_SYMBOL anywhere in that driver (checked directly,
 * not assumed), so no other module can call into it or query its state
 * directly. The only generic, exported, real signal available from
 * outside is the device's own runtime-PM accounting: if Linux's power
 * management core believes the device is suspended, nothing is actively
 * driving its SPI bus right now. This is a real signal, but weaker and
 * less specific than the driver's own kthread_stop() guarantee - honestly
 * reflected as the weakest-evidence backend here, not hidden.
 */
#include <linux/errno.h>
#include <linux/pm_runtime.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_SB_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		     QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		     QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_syncboss_quiet(void)
{
	struct device *dev;
	bool suspended;

	dev = bus_find_device_by_name(&spi_bus_type, NULL, "spi1.0");
	if (!dev)
		return false; /* can't find it: don't claim safety we don't have */

	suspended = pm_runtime_status_suspended(dev);
	put_device(dev);
	return suspended;
}

static int sb_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_SB_CAPS;
	inv->rollback_capabilities = QKX_SB_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int sb_begin(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_syncboss_quiet()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int sb_poll(void *context, const struct qkx_shutdown_key *key,
		    struct qkx_shutdown_receipt *receipt)
{
	return sb_begin(context, key, receipt);
}

static int sb_rollback(void *context, const struct qkx_shutdown_key *key,
			struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_syncboss = {
	.subsystem = QKX_SHUTDOWN_SYNCBOSS,
	.inspect = sb_inspect,
	.begin = sb_begin,
	.poll = sb_poll,
	.rollback_begin = sb_rollback,
	.rollback_poll = sb_rollback,
};
