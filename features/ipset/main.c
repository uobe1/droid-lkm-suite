// SPDX-License-Identifier: GPL-2.0-only
/*
 * dnsk_ipset: CONFIG_IP_SET core, hash:ip and hash:net set types and
 * NETFILTER_XT_SET match/target in one loadable module.
 *
 * The vendored units are Linux 6.12 upstream; see ORIGIN.md.  Registration is
 * ordered core -> set types -> xt_set, with reverse rollback.  A set keeps a
 * reference on this module through set->type->me, and an iptables/ip6tables
 * rule keeps one through the xt match/target, so rmmod fails with EBUSY while
 * either exists (upstream behaviour).
 */
#define pr_fmt(fmt) "dnsk_ipset: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>

#include "units.h"
#ifdef DNSK_XT_NOCOMPAT
#include <linux/netfilter/x_tables.h>
#include <linux/stddef.h>
_Static_assert(sizeof(struct xt_match) == 104, "device xt_match size");
_Static_assert(offsetof(struct xt_match, list) == 0, "device xt_match.list");
_Static_assert(offsetof(struct xt_match, name) == 16, "device xt_match.name");
_Static_assert(offsetof(struct xt_match, revision) == 45, "device xt_match.revision");
_Static_assert(offsetof(struct xt_match, match) == 48, "device xt_match.match");
_Static_assert(offsetof(struct xt_match, checkentry) == 56, "device xt_match.checkentry");
_Static_assert(offsetof(struct xt_match, destroy) == 64, "device xt_match.destroy");
_Static_assert(offsetof(struct xt_match, me) == 72, "device xt_match.me");
_Static_assert(offsetof(struct xt_match, table) == 80, "device xt_match.table");
_Static_assert(offsetof(struct xt_match, matchsize) == 88, "device xt_match.matchsize");
_Static_assert(offsetof(struct xt_match, usersize) == 92, "device xt_match.usersize");
_Static_assert(offsetof(struct xt_match, hooks) == 96, "device xt_match.hooks");
_Static_assert(offsetof(struct xt_match, proto) == 100, "device xt_match.proto");
_Static_assert(offsetof(struct xt_match, family) == 102, "device xt_match.family");
_Static_assert(sizeof(struct xt_target) == 104, "device xt_target size");
_Static_assert(offsetof(struct xt_target, target) == 48, "device xt_target.target");
_Static_assert(offsetof(struct xt_target, targetsize) == 88, "device xt_target.targetsize");
_Static_assert(offsetof(struct xt_target, family) == 102, "device xt_target.family");
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0) || \
	LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#error "dnsk_ipset vendors Linux 6.12 ipset/xt_set sources; build only for 6.12 GKI"
#endif

static bool registered;

static int __init dnsk_ipset_init(void)
{
	int err;

	err = dnsk_ipset_core_init();
	if (err) {
		pr_err("ipset core registration failed: %d\n", err);
		return err;
	}

	err = dnsk_ipset_hash_ip_init();
	if (err) {
		pr_err("hash:ip type registration failed: %d\n", err);
		goto err_hash_ip;
	}

	err = dnsk_ipset_hash_net_init();
	if (err) {
		pr_err("hash:net type registration failed: %d\n", err);
		goto err_hash_net;
	}

	err = dnsk_ipset_xt_set_init();
	if (err) {
		pr_err("xt_set match/target registration failed: %d\n", err);
		goto err_xt_set;
	}

	registered = true;
	pr_info("ipset protocol 7 with hash:ip, hash:net and xt_set registered\n");
	return 0;

err_xt_set:
	dnsk_ipset_hash_net_exit();
err_hash_net:
	dnsk_ipset_hash_ip_exit();
err_hash_ip:
	dnsk_ipset_core_exit();
	return err;
}

static void __exit dnsk_ipset_exit(void)
{
	if (!registered)
		return;
	registered = false;

	dnsk_ipset_xt_set_exit();
	dnsk_ipset_hash_net_exit();
	dnsk_ipset_hash_ip_exit();
	dnsk_ipset_core_exit();
}

module_init(dnsk_ipset_init);
module_exit(dnsk_ipset_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jozsef Kadlecsik <kadlec@netfilter.org>");
MODULE_DESCRIPTION("droid-lkm-suite ipset core, hash:ip/hash:net and xt_set "
		   "for android16-6.12 GKI");
