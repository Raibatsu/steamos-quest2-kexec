/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/module.h>
#include <linux/kernel.h>
#include "shutdown.h"
#include "qkx_ctx.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Real qkx_shutdown_backend implementations for Hollywood kexec safety research");
MODULE_AUTHOR("quest2-kexec-research");

static u64 qkx_epoch = 1;
static struct qkx_ctx qkx_shared_ctx = {
	.epoch = &qkx_epoch,
	.client_id = 0xCAFEUL,
};

/* Declared in each backend_*.c */
extern const struct qkx_shutdown_backend qkx_backend_camera;
extern const struct qkx_shutdown_backend qkx_backend_dsp;
extern const struct qkx_shutdown_backend qkx_backend_gpu;
extern const struct qkx_shutdown_backend qkx_backend_syncboss;
extern const struct qkx_shutdown_backend qkx_backend_usb;
extern const struct qkx_shutdown_backend qkx_backend_ufs;
extern const struct qkx_shutdown_backend qkx_backend_video;
extern const struct qkx_shutdown_backend qkx_backend_smmu;
extern const struct qkx_shutdown_backend qkx_backend_secure;
extern const struct qkx_shutdown_backend qkx_backend_aop;

static struct qkx_shutdown_backend qkx_backends[QKX_SHUTDOWN_SUBSYSTEMS];

static const char *const qkx_subsys_names[QKX_SHUTDOWN_SUBSYSTEMS] = {
	[QKX_SHUTDOWN_CAMERA] = "camera",
	[QKX_SHUTDOWN_DSP] = "adsp/cdsp/slpi",
	[QKX_SHUTDOWN_GPU] = "gpu",
	[QKX_SHUTDOWN_SYNCBOSS] = "syncboss",
	[QKX_SHUTDOWN_USB] = "usb",
	[QKX_SHUTDOWN_UFS] = "ufs",
	[QKX_SHUTDOWN_OTHER_DMA] = "video",
	[QKX_SHUTDOWN_SMMU] = "smmu",
	[QKX_SHUTDOWN_SECURE] = "secure/tz",
	[QKX_SHUTDOWN_AOP] = "aop",
};

/* Real self-test: actually calls each backend's inspect/begin with a real
 * key, reading real hardware/kernel state right now, and logs what each
 * one honestly found. This module performs no action otherwise - every
 * backend here is read-only (no writel/iowrite anywhere in this module),
 * so this self-test cannot alter any hardware or driver state.
 */
static void qkx_self_test(void)
{
	int i;

	for (i = 0; i < QKX_SHUTDOWN_SUBSYSTEMS; i++) {
		struct qkx_shutdown_inventory inv = { 0 };
		struct qkx_shutdown_receipt receipt = { 0 };
		struct qkx_shutdown_key key = { 0 };
		int ret;

		if (!qkx_backends[i].inspect) {
			pr_info("qkx_backends: [%s] no backend registered\n",
				qkx_subsys_names[i]);
			continue;
		}

		ret = qkx_backends[i].inspect(qkx_backends[i].context, &inv);
		if (ret) {
			pr_info("qkx_backends: [%s] inspect failed: %d\n",
				qkx_subsys_names[i], ret);
			continue;
		}

		key.client = inv.client;
		key.epoch = inv.epoch;
		key.transaction = 1;
		key.attempt = 1;
		key.subsystem = i;
		key.action = QKX_SHUTDOWN_CLOSE;
		key.direction = QKX_SHUTDOWN_FORWARD;

		ret = qkx_backends[i].begin(qkx_backends[i].context, &key, &receipt);
		if (ret) {
			pr_info("qkx_backends: [%s] begin failed: %d\n",
				qkx_subsys_names[i], ret);
			continue;
		}

		pr_info("qkx_backends: [%s] caps=0x%x state=%s (1=pending,2=complete)\n",
			qkx_subsys_names[i], inv.capabilities,
			receipt.state == QKX_SHUTDOWN_COMPLETE ? "COMPLETE/idle-now" :
			receipt.state == QKX_SHUTDOWN_PENDING ? "PENDING/busy-now" : "unknown");
	}
}

static int __init qkx_backends_init(void)
{
	int i;

	qkx_backends[QKX_SHUTDOWN_CAMERA] = qkx_backend_camera;
	qkx_backends[QKX_SHUTDOWN_DSP] = qkx_backend_dsp;
	qkx_backends[QKX_SHUTDOWN_GPU] = qkx_backend_gpu;
	qkx_backends[QKX_SHUTDOWN_SYNCBOSS] = qkx_backend_syncboss;
	qkx_backends[QKX_SHUTDOWN_USB] = qkx_backend_usb;
	qkx_backends[QKX_SHUTDOWN_UFS] = qkx_backend_ufs;
	qkx_backends[QKX_SHUTDOWN_OTHER_DMA] = qkx_backend_video;
	qkx_backends[QKX_SHUTDOWN_SMMU] = qkx_backend_smmu;
	qkx_backends[QKX_SHUTDOWN_SECURE] = qkx_backend_secure;
	qkx_backends[QKX_SHUTDOWN_AOP] = qkx_backend_aop;

	/* Every backend's inspect/begin/poll receives this as void *context -
	 * without it, each ctx->client_id and ctx->epoch dereference in every
	 * backend_*.c would be a NULL-pointer read the moment the real
	 * orchestrator (shutdown.c) called into any of them.
	 */
	for (i = 0; i < QKX_SHUTDOWN_SUBSYSTEMS; i++)
		qkx_backends[i].context = &qkx_shared_ctx;

	pr_info("qkx_backends: loaded. %d real subsystem backends registered "
		"(research module - does not perform a jump itself).\n",
		QKX_SHUTDOWN_SUBSYSTEMS);

	qkx_self_test();
	return 0;
}

static void __exit qkx_backends_exit(void)
{
	pr_info("qkx_backends: unloaded.\n");
}

module_init(qkx_backends_init);
module_exit(qkx_backends_exit);
