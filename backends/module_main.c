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
	return 0;
}

static void __exit qkx_backends_exit(void)
{
	pr_info("qkx_backends: unloaded.\n");
}

module_init(qkx_backends_init);
module_exit(qkx_backends_exit);
