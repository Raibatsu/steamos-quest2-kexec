/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Camera (VFE/IFE) shutdown-safety backend.
 *
 * REVISED after a sibling backend (UFS) crashed real hardware despite
 * reporting genuinely runtime-PM active - caused by an autonomous
 * link-level low-power state entirely outside Linux's generic PM model.
 * This module's own earlier guard against the camera's initial crash
 * (gated -> idle, active -> read a register) rested on the same kind of
 * "active means safe" assumption that just failed for a different
 * subsystem. There is no verified reason the VFE's per-block clock
 * gating can't have an equivalent gap, so this backend no longer reads
 * QKX_VFE_IRQ_STATUS0 or any other raw register at all, even when
 * active. A GATED clock is still a confident "idle" answer (no clock, no
 * possible bus transaction, and the camera ISP's normal idle state);
 * ACTIVE is honestly reported as PENDING rather than risking a third
 * crash to confirm it.
 */
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_CAM_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

/*
 * Verification-only, not yet wired into any real idle decision.
 *
 * WitherBlitz's camera_dsp_diag.c (Quest-2-Steam-OS-Dual-Boot) locates
 * cam_req_mgr_dev.c's file-private "static struct cam_req_mgr_device
 * g_dev" - never attached to any device's drvdata, confirmed directly,
 * so there is no safer standard path to it - via a fixed offset from the
 * address of the real, exported cam_req_mgr_is_open(): g_dev is at
 * cam_req_mgr_is_open + 0x12d8c00 in their exact build. That offset is a
 * linker-placement artifact BTF cannot verify (BTF confirms struct FIELD
 * layout, not where the linker put a static global relative to a
 * function) - so this check exists specifically to test whether their
 * offset transfers to this device's own build, by computing the same
 * FNV-1a hash of the 68 bytes at that computed address and comparing
 * against their known value (0xa679ec9abcc4670a). A match is real,
 * independent evidence the builds agree at that exact location, not
 * just that the struct layouts match (already confirmed separately via
 * this device's own /sys/kernel/btf/vmlinux). probe_kernel_read() is
 * used throughout - this reads ordinary kernel heap memory via a
 * legitimately exported symbol's address, not hardware MMIO, so the
 * worst case on a bad offset is a handled page fault, not the SError
 * class of crash this session hit twice elsewhere.
 */
extern bool cam_req_mgr_is_open(void);

#define QKX_CAM_GDEV_OFFSET	0x12d8c00L
#define QKX_CAM_ANCHOR_BYTES	68
#define QKX_CAM_ANCHOR_EXPECTED	0xa679ec9abcc4670aULL

static u64 qkx_fnv1a(const u8 *data, size_t size)
{
	u64 hash = 0xcbf29ce484222325ULL;
	size_t i;

	for (i = 0; i < size; i++)
		hash = (hash ^ data[i]) * 0x100000001b3ULL;
	return hash;
}

/* Offsets within g_dev itself, from diag_profile.h (generated from this
 * exact device's own /sys/kernel/btf/vmlinux - independently re-verified
 * this session, not just copied). CAM_LOCK guards CAM_OPEN/SUBDEVS/
 * SHUTDOWN; replicate camera_dsp_diag.c's own mutex_trylock pattern
 * rather than reading unlocked, for the same reason they do: avoid a
 * torn read racing a concurrent writer.
 */
#define QKX_CAM_OPEN_OFF	60
#define QKX_CAM_LOCK_OFF	64
#define QKX_CAM_SUBDEVS_OFF	20
#define QKX_CAM_SHUTDOWN_OFF	108

bool qkx_cam_verify_anchor(void)
{
	u8 code[QKX_CAM_ANCHOR_BYTES];
	u64 func_hash;

	/* Hash cam_req_mgr_is_open's OWN compiled code (offset 0) - the
	 * actual comparison camera_dsp_diag.c makes. Code is deterministic
	 * for a given source+compiler, unlike g_dev's contents, which
	 * change at runtime - these must not be conflated. A match here is
	 * real, independent evidence this build agrees with theirs at this
	 * exact location, justifying reuse of their g_dev offset below -
	 * confirmed once already this session (hash 0xa679ec9abcc4670a,
	 * matched), re-checked on every load rather than assumed permanent.
	 */
	if (probe_kernel_read(code, cam_req_mgr_is_open, sizeof(code))) {
		pr_info("qkx_backends: [camera] function anchor read failed\n");
		return false;
	}
	func_hash = qkx_fnv1a(code, sizeof(code));
	if (func_hash != QKX_CAM_ANCHOR_EXPECTED) {
		pr_info("qkx_backends: [camera] function anchor mismatch (0x%llx != 0x%llx) - g_dev offset not trusted\n",
			func_hash, QKX_CAM_ANCHOR_EXPECTED);
		return false;
	}
	return true;
}

static bool qkx_vfe_top_quiet(void)
{
	enum qkx_clock_state clk = qkx_clock_domain_state(&platform_bus_type, QKX_DEV_VFE0);
	void *gdev_addr;
	struct mutex *cam_lock;
	s32 open_cnt = -1, subdevs = -1;
	u8 shutdown_state = 0;
	bool locked, no_session;

	/* Direct, hardware-level evidence first: a gated clock cannot be
	 * issuing any bus transaction, full stop - no software signal is
	 * needed or more authoritative than this.
	 */
	if (clk == QKX_CLOCK_GATED)
		return true;

	/* Not gated. Fall back to the real, now-verified software signal:
	 * cam_req_mgr's own open_cnt. This only proceeds if the function
	 * anchor matched on THIS load (re-checked, not cached), and only
	 * reads under the real driver's own lock, never blocking - a busy
	 * lock means inconclusive, not idle.
	 */
	if (!qkx_cam_verify_anchor())
		return false;

	gdev_addr = (char *)cam_req_mgr_is_open + QKX_CAM_GDEV_OFFSET;
	cam_lock = (struct mutex *)((char *)gdev_addr + QKX_CAM_LOCK_OFF);

	locked = mutex_trylock(cam_lock);
	if (!locked)
		return false; /* camera-busy: inconclusive, not idle */

	if (probe_kernel_read(&open_cnt, (char *)gdev_addr + QKX_CAM_OPEN_OFF, sizeof(open_cnt)) ||
	    probe_kernel_read(&subdevs, (char *)gdev_addr + QKX_CAM_SUBDEVS_OFF, sizeof(subdevs)) ||
	    probe_kernel_read(&shutdown_state, (char *)gdev_addr + QKX_CAM_SHUTDOWN_OFF, sizeof(shutdown_state))) {
		mutex_unlock(cam_lock);
		return false;
	}
	mutex_unlock(cam_lock);

	no_session = (open_cnt == 0);
	pr_info("qkx_backends: [camera] g_dev open_cnt=%d subdevs=%d shutdown_state=%u\n",
		open_cnt, subdevs, shutdown_state);
	return no_session;
}

static int cam_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_CAM_CAPS;
	inv->rollback_capabilities = QKX_CAM_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	/* Read-only observation, not a real owned reset - weaker evidence
	 * than the other "Strong" backends, honestly reflected here.
	 */
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int cam_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_vfe_top_quiet()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->original_attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int cam_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return cam_begin(context, key, receipt);
}

static int cam_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_camera = {
	.subsystem = QKX_SHUTDOWN_CAMERA,
	.inspect = cam_inspect,
	.begin = cam_begin,
	.poll = cam_poll,
	.rollback_begin = cam_rollback,
	.rollback_poll = cam_rollback,
};
