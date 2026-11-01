// SPDX-License-Identifier: GPL-2.0-only
/*
 * Combined xtables REJECT registration.
 *
 * Linux 6.12 has no net/netfilter/xt_REJECT.c.  The iptables REJECT target is
 * split into net/ipv4/netfilter/ipt_REJECT.c (NFPROTO_IPV4) and
 * net/ipv6/netfilter/ip6t_REJECT.c (NFPROTO_IPV6).  Both files are vendored
 * verbatim except that module_init/module_exit were replaced with ds_nf_*
 * wrappers and __exit was dropped from the teardown helpers; this glue exposes
 * them as the single xt_reject unit registered from units.h so IPv4 and IPv6
 * are added and removed together.
 */
#include <linux/init.h>
#include <linux/module.h>

extern int ds_nf_init_ipt_reject(void);
extern void ds_nf_exit_ipt_reject(void);
extern int ds_nf_init_ip6t_reject(void);
extern void ds_nf_exit_ip6t_reject(void);

int __init ds_nf_init_xt_reject(void)
{
	int err;

	err = ds_nf_init_ipt_reject();
	if (err)
		return err;
	err = ds_nf_init_ip6t_reject();
	if (err)
		ds_nf_exit_ipt_reject();
	return err;
}

void ds_nf_exit_xt_reject(void)
{
	ds_nf_exit_ip6t_reject();
	ds_nf_exit_ipt_reject();
}
