/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DNSK_IPSET_UNITS_H
#define DNSK_IPSET_UNITS_H

/*
 * Upstream keeps these units in separate modules.  Here they are linked into
 * one module, so their module_init/module_exit hooks are plain functions
 * called by main.c in registration order.
 */

int dnsk_ipset_core_init(void);
void dnsk_ipset_core_exit(void);

int dnsk_ipset_hash_ip_init(void);
void dnsk_ipset_hash_ip_exit(void);

int dnsk_ipset_hash_net_init(void);
void dnsk_ipset_hash_net_exit(void);

int dnsk_ipset_xt_set_init(void);
void dnsk_ipset_xt_set_exit(void);

#endif /* DNSK_IPSET_UNITS_H */
