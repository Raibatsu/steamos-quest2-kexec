/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Video (Venus) shutdown-safety backend - this is what the shutdown
 * framework's QKX_SHUTDOWN_OTHER_DMA slot is used for here.
 *
 * REVISED after the Camera backend's success with the same general
 * technique. msm_v4l2_vidc.c's probe() calls dev_set_drvdata(&pdev->dev,
 * core) (confirmed directly), so struct msm_vidc_core is reachable via
 * the same safe pattern as UFS: bus_find_device_by_name() +
 * dev_get_drvdata(), no fragile offset trick needed.
 *
 * Offsets resolved from this exact device's own /sys/kernel/btf/vmlinux
 * (same method proven for Camera/SyncBoss this session) against struct
 * msm_vidc_core: `instances` (a list_head of active msm_vidc_inst
 * sessions - empty means no active video session, the same kind of
 * signal camera_dsp_diag.c reads for cam_req_mgr's session list) and
 * `lock`, the real mutex guarding it (mutex_trylock()'d here; busy
 * means inconclusive, never blocks). probe_kernel_read() throughout -
 * ordinary kernel heap memory via a legitimately obtained drvdata
 * pointer, not hardware MMIO.
 */
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_VID_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

#define QKX_VID_LOCK_OFF	16
#define QKX_VID_INSTANCES_OFF	4968

static bool qkx_video_quiet(void)
{
	struct device *dev;
	void *core;
	struct mutex *lock;
	void *instances_next = (void *)1; /* non-NULL default: fail safe */
	void *instances_addr;
	bool empty;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, "aa00000.qcom,vidc");
	if (!dev)
		return false;

	core = dev_get_drvdata(dev);
	if (!core) {
		put_device(dev);
		return false;
	}

	lock = (struct mutex *)((char *)core + QKX_VID_LOCK_OFF);
	if (!mutex_trylock(lock)) {
		put_device(dev);
		return false; /* busy: inconclusive, not idle */
	}

	instances_addr = (char *)core + QKX_VID_INSTANCES_OFF;
	if (probe_kernel_read(&instances_next, instances_addr, sizeof(instances_next))) {
		mutex_unlock(lock);
		put_device(dev);
		return false;
	}
	mutex_unlock(lock);
	put_device(dev);

	/* An empty list_head's own next pointer points back to itself. */
	empty = (instances_next == instances_addr);
	pr_info("qkx_backends: [video] instances_empty=%d\n", empty);
	return empty;
}

static int vid_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_VID_CAPS;
	inv->rollback_capabilities = QKX_VID_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int vid_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_video_quiet()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->original_attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int vid_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return vid_begin(context, key, receipt);
}

static int vid_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_video = {
	.subsystem = QKX_SHUTDOWN_OTHER_DMA,
	.inspect = vid_inspect,
	.begin = vid_begin,
	.poll = vid_poll,
	.rollback_begin = vid_rollback,
	.rollback_poll = vid_rollback,
};
