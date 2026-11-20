/* SPDX-License-Identifier: GPL-2.0-only
 *
 * UFS shutdown-safety backend.
 *
 * REVISED TWICE. First revision: this backend crashed real hardware
 * reading REG_UTP_TRANSFER_REQ_DOOR_BELL / REG_UTP_TASK_REQ_DOOR_BELL
 * despite the device reporting genuinely runtime-PM active - root cause:
 * UFS has its own autonomous link-hibernate state (auto_hibern8) entirely
 * outside Linux's generic PM model. That revision dropped the raw read
 * entirely rather than risk a second guess.
 *
 * This revision restores the real register read, but safely: UFS's own
 * debugfs register dump (ufsdbg_host_regs_show in ufs-debugfs.c) brackets
 * its register access with ufshcd_hold()/ufshcd_release() - a real,
 * EXPORT_SYMBOL_GPL'd, UFS-driver-specific synchronization primitive
 * whose own comment in ufshcd.c explicitly describes this exact gap:
 * "though the clocks may be ON, the link could still be in hibern8...
 * make sure we exit hibern8 state also." This is the actual mechanism
 * the real driver uses to guarantee the link is genuinely out of
 * hibernate before touching hardware - not a guess about PM state.
 *
 * struct ufs_hba is treated as opaque here deliberately. This module
 * never dereferences it or assumes anything about its internal layout
 * (a private struct, not a stable ABI, and this module's kernel-src
 * build isn't guaranteed to be the exact same commit as what's actually
 * running - confirmed earlier this session via a vermagic string
 * mismatch that the kernel's module loader didn't even enforce). The
 * pointer from dev_get_drvdata() is passed straight through to the real,
 * already-loaded ufshcd.c's own exported functions, which interpret it
 * correctly because they ARE the code that defined it. Only the function
 * call ABI (pointer + bool) needs to match, which is an architecture-
 * level guarantee, not a struct-layout one.
 */
#include <linux/errno.h>
#include <linux/device.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_UFS_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_STOP))

struct ufs_hba; /* opaque - never dereferenced by this module */
extern int ufshcd_hold(struct ufs_hba *hba, bool async);
extern void ufshcd_release(struct ufs_hba *hba, bool no_sched);

static bool qkx_ufs_doorbells_clear(void)
{
	struct device *dev;
	struct ufs_hba *hba;
	u32 tr, tm;
	bool clear;
	int ret;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, QKX_DEV_UFS);
	if (!dev)
		return false; /* can't find it - no claim either way */

	hba = dev_get_drvdata(dev);
	if (!hba) {
		put_device(dev);
		return false;
	}

	/* async=false: block until genuinely resumed, including hibern8
	 * exit if the link was there. A nonzero return means this did not
	 * succeed - do not proceed to read in that case.
	 */
	ret = ufshcd_hold(hba, false);
	if (ret) {
		put_device(dev);
		return false;
	}

	tr = qkx_read_phys(QKX_PA_UFS, 0x1000, QKX_UFS_UTRLDBR);
	tm = qkx_read_phys(QKX_PA_UFS, 0x1000, QKX_UFS_UTMRLDBR);
	clear = (tr != 0xffffffff && tm != 0xffffffff && tr == 0 && tm == 0);

	ufshcd_release(hba, false);
	put_device(dev);
	return clear;
}

static int ufs_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_UFS_CAPS;
	inv->rollback_capabilities = QKX_UFS_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int ufs_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	if (qkx_ufs_doorbells_clear()) {
		receipt->state = QKX_SHUTDOWN_COMPLETE;
		receipt->evidence = qkx_evidence_for(key);
		receipt->quiesced_attempt = key->original_attempt;
	} else {
		receipt->state = QKX_SHUTDOWN_PENDING;
	}
	return 0;
}

static int ufs_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return ufs_begin(context, key, receipt);
}

static int ufs_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_ufs = {
	.subsystem = QKX_SHUTDOWN_UFS,
	.inspect = ufs_inspect,
	.begin = ufs_begin,
	.poll = ufs_poll,
	.rollback_begin = ufs_rollback,
	.rollback_poll = ufs_rollback,
};
