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
#include "dnsk_xt_layout.h"

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
