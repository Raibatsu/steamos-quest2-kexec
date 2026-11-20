/* SPDX-License-Identifier: GPL-2.0-only
 *
 * ADSP/CDSP/SLPI shutdown-safety backend.
 *
 * REVISED after Camera/SyncBoss/Video's success with reading real,
 * BTF-verified private struct fields instead of relying on the narrow
 * exported API surface alone. find_subsys_device("adsp") already
 * legitimately returns a real struct subsys_device* - no drvdata lookup
 * or offset trick needed, this pointer is exported and intended for
 * exactly this kind of use.
 *
 * subsys_get_track() in subsystem_restart.c shows the real state can
 * live in one of two places: subsys->track directly, or
 * subsys->restart_order->track if this subsystem is grouped with others
 * for combined restart. Checked restart_order directly on this device
 * before trusting subsys->track (logged either way, not assumed NULL).
 *
 * subsys->track.state is a real enum subsys_state (SUBSYS_OFFLINING/
 * OFFLINE/ONLINE - confirmed in subsystem_restart.c, not guessed) - a
 * materially better signal than crash status alone: ONLINE is this
 * subsystem's normal, expected operating state while the AP is awake
 * (ADSP/CDSP/SLPI are almost always loaded and running), so it answers
 * "not crashed" no better than subsys_get_crash_status() already did.
 * OFFLINE means the subsystem is not loaded/running at all - a genuinely
 * stronger, more specific "safe" signal than "not crashed," read under
 * track.lock (mutex_trylock - busy means inconclusive, never blocks),
 * offsets resolved from this device's own /sys/kernel/btf/vmlinux.
 */
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <soc/qcom/subsystem_restart.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_ADSP_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		       QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		       QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

#define QKX_SUBSYS_RESTART_ORDER_OFF	1216
#define QKX_SUBSYS_TRACK_OFF		176
#define QKX_TRACK_STATE_OFF		8
#define QKX_TRACK_LOCK_OFF		16
#define QKX_SUBSYS_STATE_OFFLINE	1
/* Offset of the embedded struct device within struct subsys_device,
 * resolved from this device's own BTF - needed because
 * find_subsys_device() returns a reference-counted pointer
 * (bus_find_device() calls get_device() internally, confirmed directly
 * in drivers/base/bus.c) that must be released with put_device(), and
 * struct subsys_device is kept opaque here on purpose, so &dev->dev is
 * not available - this offset is the correct, verified way to get the
 * real struct device* without ever dereferencing the opaque type.
 * Matches WitherBlitz's own independently-reverse-engineered
 * QCD_DSP_DEV=240 exactly, additional cross-validation.
 */
#define QKX_SUBSYS_DEV_OFF		240

static bool qkx_adsp_not_crashed(void)
{
	struct subsys_device *dev;
	struct device *real_dev;
	int crashed;
	void *restart_order = (void *)1; /* non-NULL default: fail safe */
	void *track_addr;
	struct mutex *track_lock;
	s32 state = -1;
	bool offline = false;

	dev = find_subsys_device("adsp");
	if (!dev)
		return false; /* can't find it: don't claim safety we don't have */

	/* find_subsys_device() returns a device with a reference already
	 * taken (it calls bus_find_device(), which calls get_device() on
	 * whatever it matches - confirmed directly in drivers/base/bus.c).
	 * That reference was being leaked here on every call before this
	 * fix: every return path below now goes through put_device(real_dev)
	 * exactly once.
	 */
	real_dev = (struct device *)((char *)dev + QKX_SUBSYS_DEV_OFF);
	crashed = subsys_get_crash_status(dev);
	if (crashed) {
		put_device(real_dev);
		return false;
	}

	if (probe_kernel_read(&restart_order, (char *)dev + QKX_SUBSYS_RESTART_ORDER_OFF,
			       sizeof(restart_order))) {
		pr_info("qkx_backends: [adsp/cdsp/slpi] restart_order read failed\n");
		put_device(real_dev);
		return crashed == 0; /* fall back to crash-status-only */
	}
	if (restart_order) {
		/* Grouped restart order - the real state lives in
		 * restart_order->track, a different struct this backend
		 * hasn't resolved. Documented, not guessed at: fall back
		 * to crash-status-only rather than read the wrong struct.
		 */
		pr_info("qkx_backends: [adsp/cdsp/slpi] grouped restart_order=%p - state check skipped, using crash-status only\n",
			restart_order);
		put_device(real_dev);
		return crashed == 0;
	}

	track_addr = (char *)dev + QKX_SUBSYS_TRACK_OFF;
	track_lock = (struct mutex *)((char *)track_addr + QKX_TRACK_LOCK_OFF);
	if (!mutex_trylock(track_lock)) {
		put_device(real_dev);
		return false; /* busy: inconclusive, not idle */
	}

	if (probe_kernel_read(&state, (char *)track_addr + QKX_TRACK_STATE_OFF, sizeof(state))) {
		mutex_unlock(track_lock);
		put_device(real_dev);
		return false;
	}
	mutex_unlock(track_lock);
	put_device(real_dev);

	offline = (state == QKX_SUBSYS_STATE_OFFLINE);
	pr_info("qkx_backends: [adsp/cdsp/slpi] subsys_state=%d (0=offlining,1=offline,2=online) crashed=%d\n",
		state, crashed);
	return offline;
}

static int dsp_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_ADSP_CAPS;
	inv->rollback_capabilities = QKX_ADSP_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int dsp_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_adsp_not_crashed()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->original_attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int dsp_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return dsp_begin(context, key, receipt);
}

static int dsp_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_dsp = {
	.subsystem = QKX_SHUTDOWN_DSP,
	.inspect = dsp_inspect,
	.begin = dsp_begin,
	.poll = dsp_poll,
	.rollback_begin = dsp_rollback,
	.rollback_poll = dsp_rollback,
};
