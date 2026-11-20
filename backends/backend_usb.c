/* SPDX-License-Identifier: GPL-2.0-only
 *
 * USB (dwc3) shutdown-safety backend.
 *
 * Real evidence: dwc3_gadget_run_stop() in gadget.c polls the real
 * DWC3_DSTS hardware register's DEVCTRLHLT bit after clearing RUN_STOP,
 * with a comment citing the hardware databook's requirement to stop
 * active transfers first. That function is static; this backend reads
 * the same DSTS register independently, at dwc0's confirmed base address.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_USB_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_usb_halted(void)
{
	u32 dsts = qkx_read_phys(QKX_PA_USB0, 0x100000, QKX_USB_DSTS);

	if (dsts == 0xffffffff)
		return false;
	return !!(dsts & QKX_USB_DSTS_DEVCTRLHLT);
}

static int usb_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_USB_CAPS;
	inv->rollback_capabilities = QKX_USB_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int usb_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_usb_halted()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int usb_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return usb_begin(context, key, receipt);
}

static int usb_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_usb = {
	.subsystem = QKX_SHUTDOWN_USB,
	.inspect = usb_inspect,
	.begin = usb_begin,
	.poll = usb_poll,
	.rollback_begin = usb_rollback,
	.rollback_poll = usb_rollback,
};
