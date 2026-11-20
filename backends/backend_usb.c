/* SPDX-License-Identifier: GPL-2.0-only
 *
 * USB (dwc3) shutdown-safety backend.
 *
 * REVISED after a sibling backend (UFS) crashed real hardware despite its
 * owning device reporting genuinely runtime-PM active - caused by a
 * link-level autonomous low-power state entirely outside Linux's generic
 * PM model. dwc3 controllers commonly have an equivalent autonomous USB
 * link-power-state mechanism (U1/U2/U3 link states), and this driver
 * exposes no safe software-counter-based sysfs attribute to check instead
 * (checked directly: no DEVICE_ATTR for busy/link-state exists in
 * drivers/usb/dwc3/core.c). Given that, and given the now-demonstrated
 * crash class, this backend does not read DWC3_DSTS or any other raw
 * register at all. A GATED clock is still a confident "halted" answer;
 * ACTIVE is honestly reported as PENDING rather than risking a third
 * crash on real hardware to confirm it.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_USB_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

static bool qkx_usb_halted(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_USB0);

	if (clk == QKX_CLOCK_GATED)
		return true;
	return false; /* ACTIVE or UNKNOWN: not confirmed, no register read attempted */
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
