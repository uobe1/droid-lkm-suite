// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot recovery helper: drop a stale feature registration that otherwise
 * keeps the owner module pinned.  Not built or shipped by default.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <dnsk_core.h>

static char *feature = "ns";
module_param(feature, charp, 0400);
MODULE_PARM_DESC(feature, "feature name to deregister");

static int __init dnsk_unstick_init(void)
{
	struct dnsk_core_api *api;
	int ret;

	if (!feature || !feature[0])
		return -EINVAL;

	api = dnsk_core_get(DNSK_CORE_ABI, sizeof(*api));
	if (IS_ERR(api))
		return PTR_ERR(api);

	ret = api->feature_del(feature);
	module_put(api->owner);
	pr_info("dnsk_unstick: feature_del(%s) = %d\n", feature, ret);
	return ret;
}

static void __exit dnsk_unstick_exit(void)
{
}

module_init(dnsk_unstick_init);
module_exit(dnsk_unstick_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("droid-lkm-suite one-shot feature unstick helper");
MODULE_IMPORT_NS(DROID_LKM_SUITE);
