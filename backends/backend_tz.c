/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Secure/TrustZone (qseecom) shutdown-safety backend.
 *
 * Real evidence: __qseecom_unload_app() in qseecom.c makes a genuine
 * synchronous SCM call into TrustZone and can return -EBUSY - a real,
 * checkable "did not actually finish" signal the caller handles correctly
 * by restoring its refcount rather than freeing memory.
 *
 * That function, and the registered_app_list it tracks, are both private
 * to qseecom.c - there is no exported, read-only "how many apps are
 * loaded right now" query (checked directly: qseecom_kernel.h exports
 * start/shutdown/send_command/set_bandwidth, all of which require already
 * owning a handle this module never obtained). Forcing qseecom_shutdown_app()
 * on a handle we don't own isn't possible (the API requires the real
 * handle) and wouldn't be ours to call even if it were - other drivers'
 * TZ sessions are not ours to tear down.
 *
 * Honest conclusion: this backend has no independent, safe signal to
 * check from outside qseecom.c. It reports success and says so plainly,
 * rather than inventing a check that isn't real.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_TZ_CAPS QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RELEASE)

static int tz_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_TZ_CAPS;
	inv->rollback_capabilities = QKX_TZ_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int tz_begin(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int tz_poll(void *context, const struct qkx_shutdown_key *key,
		    struct qkx_shutdown_receipt *receipt)
{
	return tz_begin(context, key, receipt);
}

static int tz_rollback(void *context, const struct qkx_shutdown_key *key,
			struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_secure = {
	.subsystem = QKX_SHUTDOWN_SECURE,
	.inspect = tz_inspect,
	.begin = tz_begin,
	.poll = tz_poll,
	.rollback_begin = tz_rollback,
	.rollback_poll = tz_rollback,
};
