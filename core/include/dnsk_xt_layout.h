/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_XT_LAYOUT_H
#define _DNSK_XT_LAYOUT_H

#include <linux/netfilter/x_tables.h>
#include <linux/stddef.h>

#ifdef DNSK_XT_NOCOMPAT
/* Device 6.12.30 MTK BTF: 104 bytes, no CONFIG_NETFILTER_XTABLES_COMPAT. */
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

#endif /* _DNSK_XT_LAYOUT_H */
