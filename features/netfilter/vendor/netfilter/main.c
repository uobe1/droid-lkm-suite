// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_netfilter: " fmt
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/utsname.h>
#include <generated/utsrelease.h>
#include <linux/string.h>
#include <net/net_namespace.h>
#include <net/netns/nftables.h>
#include "dnsk_xt_layout.h"

static bool activate;
static bool gki;
module_param(activate, bool, 0400);
module_param(gki, bool, 0400);
MODULE_PARM_DESC(activate, "Register nftables/xt units after target validation");
MODULE_PARM_DESC(gki, "Allow the compiled android16-6.12 GKI family");

static bool ds_gki_supported(void)
{
	const char *release = utsname()->release;

	/* The DDK test kernel reports its own release, not the phone's KMI. */
	if (of_machine_is_compatible("linux,dummy-virt") &&
	    !strcmp(release, UTS_RELEASE))
		return true;
	return gki && !strncmp(release, "6.12.", 5) &&
	       strstr(release, "-android16-");
}

unsigned int ds_nft_state_id;

/*
 * Heap allocated, not static.
 *
 * register_pernet_subsys() publishes this struct to the network namespace
 * machinery, which keeps using it after unregister (RCU readers, and per-netns
 * data allocated from ->size).  With the struct in .bss it lives in this
 * module's image, so the netns layer would dereference freed memory once the
 * module is gone -- the same failure that had to be fixed in devtmpfs by moving
 * its file_system_type to the heap.
 *
 * A small one-shot allocation per load keeps the struct alive for exactly as
 * long as anything can reach it.
 */
static struct pernet_operations *ds_nft_state_ops_p;

static struct pernet_operations *ds_nft_state_ops_get(void)
{
	if (!ds_nft_state_ops_p) {
		/*
		 * `id` and `size` are const members, so they cannot be assigned at
		 * run time.  Copy a template instead of filling field by field.
		 */
		static const struct pernet_operations tmpl = {
			.id = &ds_nft_state_id,
			.size = sizeof(struct netns_nftables),
		};

		ds_nft_state_ops_p = kmemdup(&tmpl, sizeof(tmpl), GFP_KERNEL);
	}
	return ds_nft_state_ops_p;
}
#define DS_NF_UNIT(name) \
    extern int ds_nf_init_##name(void); \
    extern void ds_nf_exit_##name(void);
#include "units.h"
#undef DS_NF_UNIT
struct ds_nf_unit { const char *name; int (*init)(void); void (*exit)(void); };
static struct ds_nf_unit units[] = {
#define DS_NF_UNIT(name) { #name, ds_nf_init_##name, ds_nf_exit_##name },
#include "units.h"
#undef DS_NF_UNIT
};
static bool registered;

static int __init ds_netfilter_init(void)
{
	unsigned int i;
	int err;

	if (!ds_gki_supported())
		return -EOPNOTSUPP;
	if (!activate) {
		pr_info("inactive: activate=1 and gki=1 are required\n");
		return 0;
	}
	err = register_pernet_subsys(ds_nft_state_ops_get());
    if (err)
        return err;
    for (i = 0; i < ARRAY_SIZE(units); i++) {
        err = units[i].init();
        if (err) {
            pr_err("%s registration failed: %d\n", units[i].name, err);
            while (i)
                units[--i].exit();
            unregister_pernet_subsys(ds_nft_state_ops_get());
            return err;
        }
    }
	registered = true;
    pr_info("xt_addrtype and nftables registered; xt units initialised; private per-net state\n");
    return 0;
}
static void __exit ds_netfilter_exit(void)
{
    unsigned int i;

	if (!registered)
		return;
	registered = false;
    i = ARRAY_SIZE(units);
    while (i)
        units[--i].exit();
    unregister_pernet_subsys(ds_nft_state_ops_get());
}
module_init(ds_netfilter_init);
module_exit(ds_netfilter_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shikochin");
MODULE_DESCRIPTION("droid-lkm-suite xt_addrtype, xt_REJECT/xt_RECENT and nftables for android16-6.12 GKI");
