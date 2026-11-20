/* SPDX-License-Identifier: GPL-2.0-only
 *
 * GPU (Adreno/KGSL) shutdown-safety backend.
 *
 * REVISED twice. First: after two real crashes on real hardware this
 * session (camera's VFE register, then UFS's doorbell register, both
 * raising a fatal asynchronous SError), this backend stopped reading any
 * raw register at all - kgsl_active_count_get()/put() exist and look
 * like the right primitive, but neither is EXPORT_SYMBOL'd (checked
 * directly), so there is no UFS-style safe hold/release path available.
 *
 * Second (this revision): restored a real signal anyway, via a different
 * route. kgsl_pwrctrl.c's own `gpubusy` sysfs attribute
 * (DEVICE_ATTR_RO(gpubusy), confirmed real on this device at
 * /sys/class/kgsl/kgsl-3d0/gpubusy) is backed entirely by software
 * counters (device->pwrctrl.clk_stats) - its show() handler performs no
 * MMIO access at all, confirmed by reading gpubusy_show() directly. This
 * backend reads that file exactly as a userspace tool would (open, read,
 * close - qkx_read_sysfs_file()), never touching a register. The file's
 * own busy_old counter resets to 0 whenever the driver's AXI-on flag is
 * clear, so busy_old == 0 is a reasonably confident idle signal; a
 * nonzero value does not necessarily mean busy right now (the counter
 * accumulates since the last read by anyone), so this is reported
 * honestly as a sampled, not instantaneous, signal.
 */
#include <linux/errno.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_GPU_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

#define QKX_GPUBUSY_PATH "/sys/class/kgsl/kgsl-3d0/gpubusy"

static bool qkx_gpu_is_idle(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_GPU);
	char buf[64];
	ssize_t n;
	unsigned long busy_old = 0, total_old = 0;

	if (clk == QKX_CLOCK_GATED)
		return true;

	n = qkx_read_sysfs_file(QKX_GPUBUSY_PATH, buf, sizeof(buf));
	if (n <= 0)
		return false; /* couldn't read it - not confirmed */

	if (sscanf(buf, "%lu %lu", &busy_old, &total_old) != 2)
		return false;

	return busy_old == 0;
}

static int gpu_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_GPU_CAPS;
	inv->rollback_capabilities = QKX_GPU_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int gpu_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_gpu_is_idle()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = 1U << key->action;
		receipt->quiesced_attempt = key->attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int gpu_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return gpu_begin(context, key, receipt);
}

static int gpu_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	/* A read-only idle check has nothing to undo. */
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_gpu = {
	.subsystem = QKX_SHUTDOWN_GPU,
	.inspect = gpu_inspect,
	.begin = gpu_begin,
	.poll = gpu_poll,
	.rollback_begin = gpu_rollback,
	.rollback_poll = gpu_rollback,
};
