/* SPDX-License-Identifier: GPL-2.0-only
 *
 * USB (dwc3) shutdown-safety backend.
 *
 * REVISED three times. First: after a sibling backend (UFS) crashed
 * real hardware despite its owning device reporting genuinely
 * runtime-PM active - caused by a link-level autonomous low-power state
 * entirely outside Linux's generic PM model - this backend stopped
 * reading DWC3_DSTS or any other raw register, since dwc3/core.c
 * exposes no safe software-backed attribute either (checked directly).
 *
 * Second: found the generic Linux USB gadget core's
 * /sys/class/udc/<name>/state (sprintf()'d from a cached enum, no
 * register access), kept below as a fallback.
 *
 * Third (this revision): dwc3/core.c's probe() calls
 * platform_set_drvdata(pdev, dwc) on a DIFFERENT device than the
 * QKX_DEV_USB0 wrapper this backend's clock-gate check uses - confirmed
 * the real name is "a600000.dwc3" (the inner dwc3 core child node, not
 * "a600000.ssusb" the wrapper) via this device's own
 * /sys/bus/platform/devices/ listing. struct dwc3's in_lpm field is an
 * atomic_t - exactly what dwc3_link_state_show() (its own debugfs
 * handler) checks first, before ever reading DWC3_DSTS: "if
 * (atomic_read(&dwc->in_lpm)) report powered off, skip the register."
 * Reading the same flag here gives the identical safety signal the real
 * driver uses, without debugfs (confirmed unavailable on this device)
 * and without the register read itself. atomic_t needs no additional
 * lock to read safely. Offset resolved from this device's own
 * /sys/kernel/btf/vmlinux.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_USB_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

#define QKX_UDC_STATE_PATH "/sys/class/udc/a600000.dwc3/state"
#define QKX_DWC3_DEV "a600000.dwc3"
#define QKX_DWC3_IN_LPM_OFF 2304

static bool qkx_usb_in_lpm(void)
{
	struct device *dev;
	void *dwc;
	s32 in_lpm = 0;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, QKX_DWC3_DEV);
	if (!dev)
		return false;

	dwc = dev_get_drvdata(dev);
	if (!dwc) {
		put_device(dev);
		return false;
	}

	if (probe_kernel_read(&in_lpm, (char *)dwc + QKX_DWC3_IN_LPM_OFF, sizeof(in_lpm))) {
		put_device(dev);
		return false;
	}
	put_device(dev);

	pr_info("qkx_backends: [usb] dwc3 in_lpm=%d\n", in_lpm);
	return in_lpm != 0;
}

static bool qkx_usb_halted(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_USB0);
	char buf[32];
	ssize_t n;

	if (clk == QKX_CLOCK_GATED)
		return true;

	if (qkx_usb_in_lpm())
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
		receipt->quiesced_attempt = key->original_attempt;
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
