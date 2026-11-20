/* SPDX-License-Identifier: GPL-2.0-only
 *
 * USB (dwc3) shutdown-safety backend.
 *
 * REVISED twice. First: after a sibling backend (UFS) crashed real
 * hardware despite its owning device reporting genuinely runtime-PM
 * active - caused by a link-level autonomous low-power state entirely
 * outside Linux's generic PM model - this backend stopped reading
 * DWC3_DSTS or any other raw register, since dwc3/core.c exposes no
 * safe software-backed attribute either (checked directly).
 *
 * Second (this revision): found a different, safer signal. The generic
 * Linux USB gadget core (drivers/usb/gadget/udc/core.c) exposes
 * /sys/class/udc/<name>/state (confirmed real on this device at
 * /sys/class/udc/a600000.dwc3/state) - its state_show() handler does
 * nothing but sprintf() the cached gadget->state enum, no register
 * access, not even a lock needed for the read. This is exactly the kind
 * of link-level, autonomous state the clock-gate check could not see
 * (the same category of gap that crashed UFS): USB_STATE_SUSPENDED or
 * USB_STATE_NOTATTACHED here mean the link itself is confirmed not
 * active, independent of whatever Linux's generic PM thinks. Other
 * states (e.g. "configured", the normal state while actively connected)
 * do not confirm an in-flight transfer either way, so they fall through
 * to the same honest PENDING answer as before - this backend does not
 * claim more certainty than the signal actually gives.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_USB_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

#define QKX_UDC_STATE_PATH "/sys/class/udc/a600000.dwc3/state"

static bool qkx_usb_halted(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_USB0);
	char buf[32];
	ssize_t n;

	if (clk == QKX_CLOCK_GATED)
		return true;

	n = qkx_read_sysfs_file(QKX_UDC_STATE_PATH, buf, sizeof(buf));
	if (n <= 0)
		return false; /* couldn't read it - not confirmed */

	return strstr(buf, "suspended") != NULL ||
	       strstr(buf, "not attached") != NULL;
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
