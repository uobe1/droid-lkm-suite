// SPDX-License-Identifier: GPL-2.0-only
/*
 * Legacy ip6table NAT for kernels that omit CONFIG_IP6_NF_NAT.
 *
 * Same arrays and registration order as net/ipv6/netfilter/ip6table_nat.c.
 * Every ip6_tables/nf_nat symbol here is unexported on GKI; ksud resolves
 * the imports at load time.  If a native "nat" template already exists,
 * xt_register_template() returns -EEXIST and this unit stays out of the way.
 */

#define pr_fmt(fmt) "droidspaces_netfilter: " fmt
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv6.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <linux/ipv6.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include <net/netfilter/nf_nat.h>

extern struct xt_table *xt_find_table(struct net *net, u8 af,
				      const char *name);
extern int xt_register_template(const struct xt_table *table,
				int (*table_init)(struct net *net));
extern void xt_unregister_template(const struct xt_table *table);
extern int ip6t_register_table(struct net *net, const struct xt_table *table,
			       const struct ip6t_replace *repl,
			       const struct nf_hook_ops *ops);
extern void ip6t_unregister_table_exit(struct net *net, const char *name);
extern unsigned int ip6t_do_table(void *priv, struct sk_buff *skb,
				  const struct nf_hook_state *state);
extern int nf_nat_ipv6_register_fn(struct net *net,
				   const struct nf_hook_ops *ops);
extern void nf_nat_ipv6_unregister_fn(struct net *net,
				      const struct nf_hook_ops *ops);

struct ds_ip6table_nat_pernet {
	struct nf_hook_ops *nf_nat_ops;
};

static unsigned int ds_ip6table_nat_net_id __read_mostly;

static const struct xt_table ds_nf_nat_ipv6_table = {
	.name		= "nat",
	.valid_hooks	= (1 << NF_INET_PRE_ROUTING) |
			  (1 << NF_INET_POST_ROUTING) |
			  (1 << NF_INET_LOCAL_OUT) |
			  (1 << NF_INET_LOCAL_IN),
	.me		= THIS_MODULE,
	.af		= NFPROTO_IPV6,
};

static const struct nf_hook_ops ds_nf_nat_ipv6_ops[] = {
	{
		.hook		= ip6t_do_table,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_PRE_ROUTING,
		.priority	= NF_IP6_PRI_NAT_DST,
	},
	{
		.hook		= ip6t_do_table,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_POST_ROUTING,
		.priority	= NF_IP6_PRI_NAT_SRC,
	},
	{
		.hook		= ip6t_do_table,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_LOCAL_OUT,
		.priority	= NF_IP6_PRI_NAT_DST,
	},
	{
		.hook		= ip6t_do_table,
		.pf		= NFPROTO_IPV6,
		.hooknum	= NF_INET_LOCAL_IN,
		.priority	= NF_IP6_PRI_NAT_SRC,
	},
};

static int ds_ip6t_nat_register_lookups(struct net *net)
{
	struct ds_ip6table_nat_pernet *xt_nat_net;
	struct nf_hook_ops *ops;
	struct xt_table *table;
	int i, ret;

	table = xt_find_table(net, NFPROTO_IPV6, "nat");
	if (WARN_ON_ONCE(!table))
		return -ENOENT;

	xt_nat_net = net_generic(net, ds_ip6table_nat_net_id);
	ops = kmemdup(ds_nf_nat_ipv6_ops, sizeof(ds_nf_nat_ipv6_ops),
		      GFP_KERNEL);
	if (!ops)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(ds_nf_nat_ipv6_ops); i++) {
		ops[i].priv = table;
		ret = nf_nat_ipv6_register_fn(net, &ops[i]);
		if (ret) {
			while (i)
				nf_nat_ipv6_unregister_fn(net, &ops[--i]);

			kfree(ops);
			return ret;
		}
	}

	xt_nat_net->nf_nat_ops = ops;
	return 0;
}

static void ds_ip6t_nat_unregister_lookups(struct net *net)
{
	struct ds_ip6table_nat_pernet *xt_nat_net =
		net_generic(net, ds_ip6table_nat_net_id);
	struct nf_hook_ops *ops = xt_nat_net->nf_nat_ops;
	int i;

	if (!ops)
		return;

	for (i = 0; i < ARRAY_SIZE(ds_nf_nat_ipv6_ops); i++)
		nf_nat_ipv6_unregister_fn(net, &ops[i]);

	kfree(ops);
	xt_nat_net->nf_nat_ops = NULL;
}

static int ds_ip6table_nat_table_init(struct net *net)
{
	struct ip6t_replace *repl;
	int ret;

	repl = ip6t_alloc_initial_table(&ds_nf_nat_ipv6_table);
	if (repl == NULL)
		return -ENOMEM;
	ret = ip6t_register_table(net, &ds_nf_nat_ipv6_table, repl, NULL);
	if (ret < 0) {
		kfree(repl);
		return ret;
	}

	ret = ds_ip6t_nat_register_lookups(net);
	if (ret < 0)
		ip6t_unregister_table_exit(net, "nat");

	kfree(repl);
	return ret;
}

static bool ds_ip6nat_owner;

static void __net_exit ds_ip6table_nat_net_pre_exit(struct net *net)
{
	if (ds_ip6nat_owner)
		ds_ip6t_nat_unregister_lookups(net);
}

static void __net_exit ds_ip6table_nat_net_exit(struct net *net)
{
	if (ds_ip6nat_owner)
		ip6t_unregister_table_exit(net, "nat");
}

static struct pernet_operations ds_ip6table_nat_net_ops = {
	.pre_exit = ds_ip6table_nat_net_pre_exit,
	.exit	= ds_ip6table_nat_net_exit,
	.id	= &ds_ip6table_nat_net_id,
	.size	= sizeof(struct ds_ip6table_nat_pernet),
};

static bool ds_ip6nat_pernet_registered;

int ds_nf_init_ip6nat(void)
{
	int ret;

	ret = register_pernet_subsys(&ds_ip6table_nat_net_ops);
	if (ret)
		return ret;
	ds_ip6nat_pernet_registered = true;

	ret = xt_register_template(&ds_nf_nat_ipv6_table,
				   ds_ip6table_nat_table_init);
	if (ret == -EEXIST) {
		/* Native ip6table_nat (or another provider) owns "nat". */
		unregister_pernet_subsys(&ds_ip6table_nat_net_ops);
		ds_ip6nat_pernet_registered = false;
		pr_info("ip6table nat already provided; keeping native\n");
		return 0;
	}
	if (ret) {
		unregister_pernet_subsys(&ds_ip6table_nat_net_ops);
		ds_ip6nat_pernet_registered = false;
		return ret;
	}

	ds_ip6nat_owner = true;
	return 0;
}

void ds_nf_exit_ip6nat(void)
{
	if (ds_ip6nat_owner) {
		ds_ip6nat_owner = false;
		xt_unregister_template(&ds_nf_nat_ipv6_table);
	}
	if (ds_ip6nat_pernet_registered) {
		ds_ip6nat_pernet_registered = false;
		unregister_pernet_subsys(&ds_ip6table_nat_net_ops);
	}
}
