/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_XT_LAYOUT_H
#define _DNSK_XT_LAYOUT_H

#include <linux/module.h>
#include <linux/netfilter/x_tables.h>
#include <linux/stddef.h>
#include <linux/stringify.h>

/*
 * CONFIG_NETFILTER_XTABLES_COMPAT is not part of the GKI KMI.  A kernel built
 * with it has 128-byte xt_match/xt_target; one without has 104-byte copies.
 * The build profile selects the layout and the module stamps it so dnskctl can
 * reject a module built for the other layout before insmod.
 */
#ifndef DNSK_XT_LAYOUT
#ifdef DNSK_XT_NOCOMPAT
#define DNSK_XT_LAYOUT 104
#else
#define DNSK_XT_LAYOUT 128
#endif
#endif

#if defined(DNSK_XT_NOCOMPAT) && DNSK_XT_LAYOUT != 104
#error "DNSK_XT_NOCOMPAT requires DNSK_XT_LAYOUT=104"
#endif
#if !defined(DNSK_XT_NOCOMPAT) && DNSK_XT_LAYOUT != 128
#error "compat build requires DNSK_XT_LAYOUT=128"
#endif

#if DNSK_XT_LAYOUT == 104
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
_Static_assert(offsetof(struct xt_target, list) == 0, "device xt_target.list");
_Static_assert(offsetof(struct xt_target, name) == 16, "device xt_target.name");
_Static_assert(offsetof(struct xt_target, revision) == 45, "device xt_target.revision");
_Static_assert(offsetof(struct xt_target, target) == 48, "device xt_target.target");
_Static_assert(offsetof(struct xt_target, checkentry) == 56, "device xt_target.checkentry");
_Static_assert(offsetof(struct xt_target, destroy) == 64, "device xt_target.destroy");
_Static_assert(offsetof(struct xt_target, me) == 72, "device xt_target.me");
_Static_assert(offsetof(struct xt_target, table) == 80, "device xt_target.table");
_Static_assert(offsetof(struct xt_target, targetsize) == 88, "device xt_target.targetsize");
_Static_assert(offsetof(struct xt_target, usersize) == 92, "device xt_target.usersize");
_Static_assert(offsetof(struct xt_target, hooks) == 96, "device xt_target.hooks");
_Static_assert(offsetof(struct xt_target, proto) == 100, "device xt_target.proto");
_Static_assert(offsetof(struct xt_target, family) == 102, "device xt_target.family");
#elif DNSK_XT_LAYOUT == 128
/* Stock GKI 6.12 keeps CONFIG_NETFILTER_XTABLES_COMPAT=y. */
_Static_assert(sizeof(struct xt_match) == 128, "compat xt_match size");
_Static_assert(offsetof(struct xt_match, list) == 0, "compat xt_match.list");
_Static_assert(offsetof(struct xt_match, name) == 16, "compat xt_match.name");
_Static_assert(offsetof(struct xt_match, revision) == 45, "compat xt_match.revision");
_Static_assert(offsetof(struct xt_match, match) == 48, "compat xt_match.match");
_Static_assert(offsetof(struct xt_match, checkentry) == 56, "compat xt_match.checkentry");
_Static_assert(offsetof(struct xt_match, destroy) == 64, "compat xt_match.destroy");
_Static_assert(offsetof(struct xt_match, compat_from_user) == 72, "compat xt_match.compat_from_user");
_Static_assert(offsetof(struct xt_match, compat_to_user) == 80, "compat xt_match.compat_to_user");
_Static_assert(offsetof(struct xt_match, me) == 88, "compat xt_match.me");
_Static_assert(offsetof(struct xt_match, table) == 96, "compat xt_match.table");
_Static_assert(offsetof(struct xt_match, matchsize) == 104, "compat xt_match.matchsize");
_Static_assert(offsetof(struct xt_match, usersize) == 108, "compat xt_match.usersize");
_Static_assert(offsetof(struct xt_match, compatsize) == 112, "compat xt_match.compatsize");
_Static_assert(offsetof(struct xt_match, hooks) == 116, "compat xt_match.hooks");
_Static_assert(offsetof(struct xt_match, proto) == 120, "compat xt_match.proto");
_Static_assert(offsetof(struct xt_match, family) == 122, "compat xt_match.family");

_Static_assert(sizeof(struct xt_target) == 128, "compat xt_target size");
_Static_assert(offsetof(struct xt_target, list) == 0, "compat xt_target.list");
_Static_assert(offsetof(struct xt_target, name) == 16, "compat xt_target.name");
_Static_assert(offsetof(struct xt_target, revision) == 45, "compat xt_target.revision");
_Static_assert(offsetof(struct xt_target, target) == 48, "compat xt_target.target");
_Static_assert(offsetof(struct xt_target, checkentry) == 56, "compat xt_target.checkentry");
_Static_assert(offsetof(struct xt_target, destroy) == 64, "compat xt_target.destroy");
_Static_assert(offsetof(struct xt_target, compat_from_user) == 72, "compat xt_target.compat_from_user");
_Static_assert(offsetof(struct xt_target, compat_to_user) == 80, "compat xt_target.compat_to_user");
_Static_assert(offsetof(struct xt_target, me) == 88, "compat xt_target.me");
_Static_assert(offsetof(struct xt_target, table) == 96, "compat xt_target.table");
_Static_assert(offsetof(struct xt_target, targetsize) == 104, "compat xt_target.targetsize");
_Static_assert(offsetof(struct xt_target, usersize) == 108, "compat xt_target.usersize");
_Static_assert(offsetof(struct xt_target, compatsize) == 112, "compat xt_target.compatsize");
_Static_assert(offsetof(struct xt_target, hooks) == 116, "compat xt_target.hooks");
_Static_assert(offsetof(struct xt_target, proto) == 120, "compat xt_target.proto");
_Static_assert(offsetof(struct xt_target, family) == 122, "compat xt_target.family");
#else
#error "unsupported DNSK_XT_LAYOUT"
#endif

MODULE_INFO(dnsk_xt_layout, __stringify(DNSK_XT_LAYOUT));

#endif /* _DNSK_XT_LAYOUT_H */
