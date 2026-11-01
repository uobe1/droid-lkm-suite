/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Device-layout shim: the 6.12.30 MTK kernel has 104-byte xt_match/xt_target
 * without CONFIG_COMPAT members, while the DDK headers add compat_from_user/
 * compat_to_user (128 bytes).  This shim is first on LINUXINCLUDE only for
 * DNSK_XT_NOCOMPAT builds; it removes the compat members before the real
 * kernel header is parsed.
 */
#ifndef DNSK_XTABLES_SHIM_H
#define DNSK_XTABLES_SHIM_H
#ifdef CONFIG_COMPAT
#undef CONFIG_COMPAT
#endif
#include_next <linux/netfilter/x_tables.h>
#endif
