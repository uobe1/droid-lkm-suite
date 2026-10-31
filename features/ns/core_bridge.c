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
	unsigned long (*module_lookup)(const char *);
	struct dnsk_feature_desc desc = {
		.name = "ns",
		.version = "m1",
		.status = DNSK_STATUS_DEVELOPMENT,
		.caps = "pid_ns,ipc_ns,sysvipc,posix_mqueue",
		.owner = THIS_MODULE,
	};
	unsigned long addr;
	int ret;

	/* dnsk_core_get lives in a module, so the core-kernel kallsyms walk
	 * cannot see it; module_kallsyms_lookup_name searches loaded modules. */
	module_lookup = (void *)kallsyms_name_to_addr("module_kallsyms_lookup_name");
	addr = module_lookup ? module_lookup("dnsk_core_get") : 0;
	if (!addr)
		addr = kallsyms_name_to_addr("dnsk_core_get");
	if (!addr)
		return -ENOENT;
	get_api = (void *)addr;

	dnsk_api = get_api(DNSK_CORE_ABI, sizeof(*dnsk_api));
	if (IS_ERR(dnsk_api)) {
		ret = PTR_ERR(dnsk_api);
		dnsk_api = NULL;
		return ret;
	}
	ret = dnsk_api->feature_add(&desc);
	if (ret) {
		module_put(dnsk_api->owner);
		dnsk_api = NULL;
	}
	return ret;
}

void dnsk_ns_core_unregister(void)
{
	struct module *owner;

	if (!dnsk_api)
		return;

	owner = dnsk_api->owner;
	dnsk_api->feature_del("ns");
	dnsk_api = NULL;
	module_put(owner);
}
