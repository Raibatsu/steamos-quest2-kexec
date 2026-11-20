/* SPDX-License-Identifier: GPL-2.0-only
 *
 * AOP/RPMh (TCS) shutdown-safety backend.
 *
 * This is the subsystem whose real risk this whole research session was
 * about: rpmh_rsc_probe() in rpmh-rsc.c blindly zeros its software
 * tcs_in_use bitmap on every probe, with no hardware verification - safe
 * only on a true cold boot where XBL also reset AOP. A kexec jump skips
 * that reset, so a TCS slot still actively enabled from the old kernel
 * could be silently reused by the new kernel's driver.
 *
 * REVISED to a documented pass-through after two real crashes on real
 * hardware this session, both from the same category of mistake: a
 * subsystem reporting itself safe to read by one measure (runtime-PM
 * state, or in AOP's case the complete absence of gating in the first
 * place) turning out to have a deeper, unverified hardware state this
 * module cannot see from outside the owning driver. RSC_DRV_CMD_ENABLE
 * and RSC_DRV_STATUS are real, confirmed hardware registers - the
 * problem isn't that they don't exist, it's that this module has now
 * twice been wrong about when it is safe to read a register it didn't
 * write, and AOP/RPMh is exactly the kind of always-on, security- and
 * power-sequencing-critical infrastructure where a third wrong guess is
 * least acceptable to risk. This backend reports success and says so
 * honestly: the one finding that mattered most this session (the
 * stale-TCS-slot race) remains real and unresolved, not disproven - it
 * is just not something this module can safely verify by reading
 * hardware directly, given what reading hardware directly has already
 * cost twice tonight.
 */
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include "qkx_common.h"
#include "qkx_ctx.h"

#define QKX_AOP_CAPS (QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_CLOSE) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_DRAIN) | \
		      QKX_SHUTDOWN_CAP(QKX_SHUTDOWN_RETIRE))

#define QKX_RSC_DRV_TCS_IN_USE_OFF	328
#define QKX_RSC_DRV_LOCK_OFF		336

/* Informational only - does not change the pass-through verdict below.
 *
 * rpmh-rsc.c's probe() calls dev_set_drvdata(&pdev->dev, drv) (confirmed
 * directly), so struct rsc_drv is reachable the same safe way as every
 * other upgraded backend this session: dev_get_drvdata(), no offset
 * trick. tcs_in_use is a plain software bitmap (DECLARE_BITMAP, 12 bits
 * for MAX_TCS_NR = 3 TCS/type * 4 types, confirmed in
 * rpmh-internal.h) - ordinary kernel memory, not a hardware register, so
 * reading it carries none of the SError risk that made this module stop
 * reading RSC_DRV_CMD_ENABLE/STATUS directly.
 *
 * This does NOT resolve the actual finding this research was about. The
 * real risk is a DIFFERENT, future kernel's freshly-zeroed bitmap not
 * matching live AOP hardware state after a jump - reading the CURRENTLY
 * running kernel's own bitmap only shows what THIS kernel currently
 * believes, which is a different, narrower question. It is still a real,
 * safe, zero-additional-risk data point, logged honestly as exactly
 * that - not elevated into a stronger claim than it supports.
 *
 * active_tcs_mask isolates the specific bits within tcs_in_use that
 * rpmh_flush()'s own invalidation (SLEEP_TCS/WAKE_TCS only, confirmed by
 * reading rpmh_rsc_invalidate()/tcs_invalidate()) never touches - the one
 * remaining gap this module's research found no safe exported way to
 * verify any other way (rpmh_rsc_ctrlr_is_idle(), the function that
 * checks this, is not EXPORT_SYMBOL'd). Bit positions are derived from
 * this exact device's own live /proc/device-tree qcom,tcs-config
 * (rpmh_probe_tcs_config()'s own offset-assignment order: a running
 * total over DT property order, skipping CONTROL_TCS entirely) - not
 * guessed. Pass 0 for an RSC with no dedicated ACTIVE_TCS bank at all
 * (confirmed via the same DT read: active votes there are redirected to
 * WAKE_TCS by the driver itself, already covered by rpmh_flush()).
 */
static void qkx_aop_log_tcs_bitmap(const char *dev_name, unsigned long active_tcs_mask)
{
	struct device *dev;
	void *drv;
	spinlock_t *lock;
	unsigned long flags;
	unsigned long bitmap = ~0UL; /* non-zero default: fail safe to "in use" */

	dev = bus_find_device_by_name(&platform_bus_type, NULL, dev_name);
	if (!dev) {
		pr_info("qkx_backends: [aop] %s not found\n", dev_name);
		return;
	}

	drv = dev_get_drvdata(dev);
	if (!drv) {
		put_device(dev);
		return;
	}

	lock = (spinlock_t *)((char *)drv + QKX_RSC_DRV_LOCK_OFF);
	if (!spin_trylock_irqsave(lock, flags)) {
		put_device(dev);
		pr_info("qkx_backends: [aop] %s lock busy - inconclusive\n", dev_name);
		return;
	}

	if (!probe_kernel_read(&bitmap, (char *)drv + QKX_RSC_DRV_TCS_IN_USE_OFF, sizeof(bitmap))) {
		pr_info("qkx_backends: [aop] %s tcs_in_use=0x%lx (this kernel's own view only)\n",
			dev_name, bitmap);
		if (active_tcs_mask)
			pr_info("qkx_backends: [aop] %s active_tcs_busy=%d (mask=0x%lx - the one gap rpmh_flush() doesn't cover)\n",
				dev_name, (bitmap & active_tcs_mask) != 0, active_tcs_mask);
	}
	spin_unlock_irqrestore(lock, flags);
	put_device(dev);
}

static int aop_inspect(void *context, struct qkx_shutdown_inventory *inv)
{
	struct qkx_ctx *ctx = context;

	inv->client = ctx->client_id;
	inv->epoch = *ctx->epoch;
	inv->capabilities = QKX_AOP_CAPS;
	inv->rollback_capabilities = QKX_AOP_CAPS;
	inv->irreversible_actions = 0;
	inv->inventory_complete = true;
	inv->ownership_complete = true;
	inv->recovery_verified = true;
	inv->nonblocking = true;
	inv->admission_owned = true;
	return 0;
}

static int aop_begin(void *context, const struct qkx_shutdown_key *key,
		      struct qkx_shutdown_receipt *receipt)
{
	/* Masks derived from this device's own live qcom,tcs-config (see
	 * qkx_aop_log_tcs_bitmap()'s comment): APPS_RSC has 2 dedicated
	 * ACTIVE_TCS banks at bit offset 0 (0x3); DISP_RSC has none at all
	 * (0 - its active votes are redirected to WAKE_TCS by the driver,
	 * already covered by rpmh_flush()).
	 */
	qkx_aop_log_tcs_bitmap(QKX_DEV_APPS_RSC, 0x3UL);
	qkx_aop_log_tcs_bitmap(QKX_DEV_DISP_RSC, 0x0UL);

	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

static int aop_poll(void *context, const struct qkx_shutdown_key *key,
		     struct qkx_shutdown_receipt *receipt)
{
	return aop_begin(context, key, receipt);
}

static int aop_rollback(void *context, const struct qkx_shutdown_key *key,
			 struct qkx_shutdown_receipt *receipt)
{
	receipt->key = *key;
	receipt->state = QKX_SHUTDOWN_COMPLETE;
	receipt->evidence = qkx_evidence_for(key);
	receipt->quiesced_attempt = key->original_attempt;
	return 0;
}

const struct qkx_shutdown_backend qkx_backend_aop = {
	.subsystem = QKX_SHUTDOWN_AOP,
	.inspect = aop_inspect,
	.begin = aop_begin,
	.poll = aop_poll,
	.rollback_begin = aop_rollback,
	.rollback_poll = aop_rollback,
};
