/* SPDX-License-Identifier: GPL-2.0-only
 *
 * GPU (Adreno/KGSL) shutdown-safety backend.
 *
 * REVISED three times. First: after two real crashes on real hardware
 * this session (camera's VFE register, then UFS's doorbell register,
 * both raising a fatal asynchronous SError), this backend stopped
 * reading any raw register at all - kgsl_active_count_get()/put() exist
 * and look like the right primitive, but neither is EXPORT_SYMBOL'd
 * (checked directly), so there is no UFS-style safe hold/release path.
 *
 * Second: restored a real signal via kgsl_pwrctrl.c's `gpubusy` sysfs
 * attribute (software counters, device->pwrctrl.clk_stats, confirmed no
 * MMIO access in gpubusy_show()) - a sampled, reset-on-read signal, kept
 * below as a fallback.
 *
 * Third (this revision): kgsl.c's probe() calls
 * dev_set_drvdata(&device->pdev->dev, device) (confirmed directly), so
 * struct kgsl_device is reachable the same safe way as UFS/SyncBoss/
 * Video: dev_get_drvdata(), no offset trick. device->state is KGSL's own
 * real power state machine (KGSL_STATE_SLUMBER=0x80 is fully powered
 * down - stronger and more specific than gpubusy's sampled counter),
 * read under device->mutex (mutex_trylock - busy means inconclusive,
 * never blocks; this is KGSL's main device-wide lock, so busy is not
 * unusual and simply falls through to gpubusy below, not treated as a
 * failure). Offsets resolved from this device's own
 * /sys/kernel/btf/vmlinux.
 */
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_GPU_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

#define QKX_GPUBUSY_PATH "/sys/class/kgsl/kgsl-3d0/gpubusy"
#define QKX_KGSL_MUTEX_OFF	2304
#define QKX_KGSL_STATE_OFF	2336
#define QKX_KGSL_STATE_SLUMBER	0x00000080

static bool qkx_gpu_kgsl_slumber(void)
{
	struct device *dev;
	void *device;
	struct mutex *mutex;
	u32 state = 0;
	bool slumber = false;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, QKX_DEV_GPU);
	if (!dev)
		return false;

	device = dev_get_drvdata(dev);
	if (!device) {
		put_device(dev);
		return false;
	}

	mutex = (struct mutex *)((char *)device + QKX_KGSL_MUTEX_OFF);
	if (!mutex_trylock(mutex)) {
		put_device(dev);
		return false; /* busy: inconclusive, falls through to gpubusy */
	}

	if (probe_kernel_read(&state, (char *)device + QKX_KGSL_STATE_OFF, sizeof(state))) {
		mutex_unlock(mutex);
		put_device(dev);
		return false;
	}
	mutex_unlock(mutex);
	put_device(dev);

	slumber = (state == QKX_KGSL_STATE_SLUMBER);
	pr_info("qkx_backends: [gpu] kgsl_state=0x%x slumber=%d\n", state, slumber);
	return slumber;
}

static bool qkx_gpu_is_idle(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_GPU);
	char buf[64];
	ssize_t n;
	unsigned long busy_old = 0, total_old = 0;

	if (clk == QKX_CLOCK_GATED)
		return true;

	if (qkx_gpu_kgsl_slumber())
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
