// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_netfilter: " fmt
#include <linux/module.h>
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
static struct pernet_operations ds_nft_state_ops = {
    .id = &ds_nft_state_id,
    .size = sizeof(struct netns_nftables),
};
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
	err = register_pernet_subsys(&ds_nft_state_ops);
    if (err)
        return err;
    for (i = 0; i < ARRAY_SIZE(units); i++) {
        err = units[i].init();
        if (err) {
            pr_err("%s registration failed: %d\n", units[i].name, err);
            while (i)
                units[--i].exit();
            unregister_pernet_subsys(&ds_nft_state_ops);
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
    unregister_pernet_subsys(&ds_nft_state_ops);
}
module_init(ds_netfilter_init);
module_exit(ds_netfilter_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shikochin");
MODULE_DESCRIPTION("droid-lkm-suite xt_addrtype, xt_REJECT/xt_RECENT and nftables for android16-6.12 GKI");
