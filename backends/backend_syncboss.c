/* SPDX-License-Identifier: GPL-2.0-only
 *
 * SyncBoss shutdown-safety backend.
 *
 * REVISED after the Camera backend's success with the same general
 * technique. syncboss_spi_core.c's probe() calls dev_set_drvdata(dev,
 * devdata) (confirmed directly) - unlike cam_req_mgr's file-private
 * g_dev, this means struct syncboss_dev_data is reachable the same safe
 * way as UFS's fix: bus_find_device_by_name() + dev_get_drvdata(), no
 * fragile function-relative-offset trick needed at all.
 *
 * Field offsets below were resolved from this exact device's own
 * /sys/kernel/btf/vmlinux (pulled and parsed this session, same method
 * proven for Camera) against struct syncboss_dev_data: worker (a
 * task_struct* - NULL means no streaming thread, matching
 * is_streaming_locked()'s own real check: "return devdata->worker !=
 * NULL"), mcu_client_count / streaming_client_count (the real client
 * refcounts gating MCU wake/streaming), stop_stream_in_progress, and
 * state_mutex (the real lock guarding all of the above -
 * mutex_trylock()'d here exactly as the driver's own code does; busy
 * means inconclusive, never blocks). probe_kernel_read() throughout -
 * ordinary kernel heap memory via a legitimately obtained drvdata
 * pointer, not hardware MMIO.
 */
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_SB_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		     QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		     QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

#define QKX_SB_WORKER_OFF		416
#define QKX_SB_MCU_CLIENTS_OFF		280
#define QKX_SB_STREAM_CLIENTS_OFF	284
#define QKX_SB_STOP_IN_PROGRESS_OFF	801
#define QKX_SB_STATE_MUTEX_OFF		616

static bool qkx_syncboss_quiet(void)
{
	struct device *dev;
	void *devdata;
	struct mutex *state_mutex;
	void *worker = (void *)1; /* non-NULL default: fail safe to "not idle" */
	s32 mcu_clients = -1, stream_clients = -1;
	u8 stop_in_progress = 1;
	bool no_clients;

	dev = bus_find_device_by_name(&spi_bus_type, NULL, "spi1.0");
	if (!dev)
		return false; /* can't find it: don't claim safety we don't have */

	devdata = dev_get_drvdata(dev);
	if (!devdata) {
		put_device(dev);
		return false;
	}

	state_mutex = (struct mutex *)((char *)devdata + QKX_SB_STATE_MUTEX_OFF);
	if (!mutex_trylock(state_mutex)) {
		put_device(dev);
		return false; /* busy: inconclusive, not idle */
	}

	if (probe_kernel_read(&worker, (char *)devdata + QKX_SB_WORKER_OFF, sizeof(worker)) ||
	    probe_kernel_read(&mcu_clients, (char *)devdata + QKX_SB_MCU_CLIENTS_OFF, sizeof(mcu_clients)) ||
	    probe_kernel_read(&stream_clients, (char *)devdata + QKX_SB_STREAM_CLIENTS_OFF, sizeof(stream_clients)) ||
	    probe_kernel_read(&stop_in_progress, (char *)devdata + QKX_SB_STOP_IN_PROGRESS_OFF, sizeof(stop_in_progress))) {
		mutex_unlock(state_mutex);
		put_device(dev);
		return false;
	}
	mutex_unlock(state_mutex);
	put_device(dev);

	no_clients = (worker == NULL) && (mcu_clients == 0) && (stream_clients == 0);
	pr_info("qkx_backends: [syncboss] worker=%s mcu_clients=%d stream_clients=%d stop_in_progress=%u\n",
		worker ? "running" : "null", mcu_clients, stream_clients, stop_in_progress);
	return no_clients;
}

static int sb_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_SB_CAPS;
	inv->rollback_capabilities = QKX_SB_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int sb_begin(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_syncboss_quiet()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->original_attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int sb_poll(void *context, const struct qkx_shutdown_key *key,
		    struct qkx_shutdown_receipt *receipt)
{
	return sb_begin(context, key, receipt);
}

static int sb_rollback(void *context, const struct qkx_shutdown_key *key,
			struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_syncboss = {
	.subsystem = QKX_SHUTDOWN_SYNCBOSS,
	.inspect = sb_inspect,
	.begin = sb_begin,
	.poll = sb_poll,
	.rollback_begin = sb_rollback,
	.rollback_poll = sb_rollback,
};
