/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The target 6.12.30 MTK kernel has no CONFIG_COMPAT members in
 * struct xt_match/xt_target (BTF: 104 bytes), while the DDK 6.12.76 headers
 * add compat_from_user/compat_to_user (128 bytes).  A 128-byte array is
 * walked with a 104-byte stride by the device kernel and crashes in
 * xt_register_matches.  This forced include is only used by the device
 * variant (DNSK_XT_NOCOMPAT=1); the stock-GKI QEMU build keeps compat.
 */
#ifdef CONFIG_COMPAT
#undef CONFIG_COMPAT
#endif
