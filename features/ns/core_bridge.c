// SPDX-License-Identifier: GPL-2.0-only
/*
 * Optional status bridge to dnsk_core.  The symbol is resolved at runtime so
 * the namespace module still works when core is absent; a missing core only
 * costs the status row, never the namespace implementation.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <dnsk_core.h>
#include "core.h"
#include "core_bridge.h"

static struct dnsk_core_api *dnsk_api;

int dnsk_ns_core_register(void)
{
	struct dnsk_core_api *(*get_api)(u16, u16);
	struct dnsk_feature_desc desc = {
		.name = "ns",
		.version = "m1",
		.status = DNSK_STATUS_DEVELOPMENT,
		.caps = "pid_ns,ipc_ns,sysvipc,posix_mqueue",
		.owner = THIS_MODULE,
	};
	int ret;

	get_api = (void *)kallsyms_name_to_addr("dnsk_core_get");
	if (!get_api)
		return -ENOENT;
	dnsk_api = get_api(DNSK_CORE_ABI, sizeof(*dnsk_api));
	if (IS_ERR(dnsk_api)) {
		ret = PTR_ERR(dnsk_api);
		dnsk_api = NULL;
		return ret;
	}
	ret = dnsk_api->feature_add(&desc);
	if (ret)
		dnsk_api = NULL;
	return ret;
}

void dnsk_ns_core_unregister(void)
{
	if (dnsk_api) {
		dnsk_api->feature_del("ns");
		dnsk_api = NULL;
	}
}
