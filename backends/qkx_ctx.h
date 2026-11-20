/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QKX_CTX_H
#define QKX_CTX_H

#include <linux/types.h>

/* Shared context every backend's inspect/begin/poll receives. epoch is
 * owned by module_main.c and set once per shutdown attempt; backends never
 * write it, only read it, matching qkx_shutdown_inspect()'s requirement
 * that every subsystem's inventory report the same plan epoch.
 */
struct qkx_ctx {
	u64 *epoch;
	u64 client_id;
};

#endif
