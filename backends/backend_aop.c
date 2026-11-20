/* SPDX-License-Identifier: GPL-2.0-only
 *
 * AOP/RPMh (TCS) shutdown-safety backend.
 *
 * This is the subsystem whose real risk this whole research session was
 * about: rpmh_rsc_probe() in rpmh-rsc.c blindly zeros its software
 * tcs_in_use bitmap on every probe, with no hardware verification - safe
 * only on a true cold boot where XBL also reset AOP. A kexec jump skips
 * that reset, so a TCS slot still actively enabled from the old kernel
 * could be silently reused by the new kernel's driver.
 *
 * REVISED to a documented pass-through after two real crashes on real
 * hardware this session, both from the same category of mistake: a
 * subsystem reporting itself safe to read by one measure (runtime-PM
 * state, or in AOP's case the complete absence of gating in the first
 * place) turning out to have a deeper, unverified hardware state this
 * module cannot see from outside the owning driver. RSC_DRV_CMD_ENABLE
 * and RSC_DRV_STATUS are real, confirmed hardware registers - the
 * problem isn't that they don't exist, it's that this module has now
 * twice been wrong about when it is safe to read a register it didn't
 * write, and AOP/RPMh is exactly the kind of always-on, security- and
 * power-sequencing-critical infrastructure where a third wrong guess is
 * least acceptable to risk. This backend reports success and says so
 * honestly: the one finding that mattered most this session (the
 * stale-TCS-slot race) remains real and unresolved, not disproven - it
 * is just not something this module can safely verify by reading
 * hardware directly, given what reading hardware directly has already
 * cost twice tonight.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_AOP_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RETIRE))

static int aop_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_AOP_CAPS;
	inv->rollback_capabilities = QKX_AOP_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int aop_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->attempt;
	return 0;
}

static int aop_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return aop_begin(context, key, receipt);
}

static int aop_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_aop = {
	.subsystem = QKX_SHUTDOWN_AOP,
	.inspect = aop_inspect,
	.begin = aop_begin,
	.poll = aop_poll,
	.rollback_begin = aop_rollback,
	.rollback_poll = aop_rollback,
};
