// SPDX-License-Identifier: GPL-2.0-only
/*
 * dnsk_core: control plane for droid-lkm-suite feature modules.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stdarg.h>
#include "dnsk_internal.h"

static bool dnsk_core_ready;

static void *dnsk_api_resolve(const char *name)
{
	return dnsk_resolve_symbol(name);
}

static int dnsk_api_hook_add(const struct dnsk_hook_req *req)
{
	return dnsk_hook_add_internal(req);
}

static int dnsk_api_hook_del(const char *key)
{
	return dnsk_hook_del_internal(key);
}

static int dnsk_api_feature_add(const struct dnsk_feature_desc *desc)
{
	return dnsk_feature_add_internal(desc);
}

static int dnsk_api_feature_del(const char *name)
{
	return dnsk_feature_del_internal(name);
}

static void dnsk_api_log(int level, const char *fmt, ...)
{
	char buf[256];
	va_list args;

	if (!fmt)
		return;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	dnsk_log(level, "%s", buf);
}

static struct dnsk_core_api dnsk_api = {
	.magic = DNSK_CORE_MAGIC,
	.abi = DNSK_CORE_ABI,
	.size = sizeof(struct dnsk_core_api),
	.resolve = dnsk_api_resolve,
	.hook_add = dnsk_api_hook_add,
	.hook_del = dnsk_api_hook_del,
	.feature_add = dnsk_api_feature_add,
	.feature_del = dnsk_api_feature_del,
	.log = dnsk_api_log,
};

struct dnsk_core_api *dnsk_core_get(u16 abi, u16 size)
{
	if (!dnsk_core_ready)
		return ERR_PTR(-ENODEV);
	if (abi != DNSK_CORE_ABI || size < sizeof(struct dnsk_core_api))
		return ERR_PTR(-EPROTO);
	return &dnsk_api;
}
EXPORT_SYMBOL_NS_GPL(dnsk_core_get, DROID_LKM_SUITE);

static int __init dnsk_core_init(void)
{
	int ret;

	ret = dnsk_symbols_init();
	if (ret)
		return ret;

	ret = dnsk_hooks_init();
	if (ret)
		goto err_symbols;

	ret = dnsk_features_init();
	if (ret)
		goto err_hooks;

	ret = dnsk_patch_init();
	if (ret)
		dnsk_warn("patching unavailable; syscall/data hooks will fail closed\n");

	ret = dnsk_diag_init();
	if (ret)
		goto err_patch;

	if (dnsk_panic_init())
		dnsk_warn("panic markers unavailable\n");

	dnsk_core_ready = true;
	dnsk_info("core ready abi=%u patch=%s\n", DNSK_CORE_ABI,
		  dnsk_patch_ready() ? "ready" : "disabled");
	return 0;

err_patch:
	dnsk_features_exit();
err_hooks:
	dnsk_hooks_exit();
err_symbols:
	dnsk_symbols_exit();
	return ret;
}

static void __exit dnsk_core_exit(void)
{
	dnsk_core_ready = false;
	dnsk_panic_exit();
	dnsk_diag_exit();
	dnsk_features_exit();
	dnsk_hooks_exit();
	dnsk_symbols_exit();
}

module_init(dnsk_core_init);
module_exit(dnsk_core_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("droid-lkm-suite core control plane");
MODULE_AUTHOR("Uobe");
