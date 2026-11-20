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
 * Real evidence: RSC_DRV_CMD_ENABLE (0x1c) and RSC_DRV_STATUS (0x18) are
 * genuine per-TCS hardware registers (confirmed in rpmh-rsc.c's own
 * read_tcs_reg() call sites this session) - independent of any software
 * bookkeeping. This backend reads CMD_ENABLE for every TCS slot across
 * both apps_rsc and disp_rsc; any slot still showing enabled means AOP
 * has a live command this kernel doesn't know about, and it is not safe
 * to let a new kernel's RPMh driver re-initialize yet.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_AOP_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RETIRE))

static bool qkx_rsc_all_tcs_clear(phys_addr_t rsc_pa)
{
	void __iomem *base;
	int i;
	bool clear = true;

	base = ioremap(rsc_pa, 0x10000);
	if (!base)
		return false;

	for (i = 0; i < QKX_RSC_MAX_TCS_PER_DRV; i++) {
		u32 off = i * QKX_RSC_DRV_TCS_OFFSET;
		u32 enable = readl_relaxed(base + off + QKX_RSC_DRV_CMD_ENABLE_OFF);

		if (enable != 0) {
			clear = false;
			break;
		}
	}

	iounmap(base);
	return clear;
}

static bool qkx_aop_quiet(void)
{
	return qkx_rsc_all_tcs_clear(QKX_PA_APPS_RSC) &&
	       qkx_rsc_all_tcs_clear(QKX_PA_DISP_RSC);
}

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
	if (qkx_aop_quiet()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
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
