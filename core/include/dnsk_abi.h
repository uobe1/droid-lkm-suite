/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_ABI_H
#define _DNSK_ABI_H

#include <linux/module.h>
#include <linux/stringify.h>

/**
 * define DNSK_ABI_PROFILE - build profile this module was compiled for
 *
 * The build profile selects the ABI-sensitive config.  Every feature module
 * stamps the profile and the macro-able knobs so dnskctl and package.py can
 * reject a module built for a different profile.  The value is recorded in
 * modinfo as @dnsk_abi_profile.  A module built without an explicit profile
 * carries "unknown", which does not match any real profile and so fails those
 * comparisons.
 */
#ifndef DNSK_ABI_PROFILE
#define DNSK_ABI_PROFILE "unknown"
#endif

MODULE_INFO(dnsk_abi_profile, DNSK_ABI_PROFILE);
#ifdef DNSK_PAGE_SIZE
MODULE_INFO(dnsk_page_size, __stringify(DNSK_PAGE_SIZE));
#endif
#ifdef DNSK_MAX_SKB_FRAGS
MODULE_INFO(dnsk_max_skb_frags, __stringify(DNSK_MAX_SKB_FRAGS));
#endif
#ifdef DNSK_CGROUP_SUBSYS_COUNT
MODULE_INFO(dnsk_cgroup_subsys_count, __stringify(DNSK_CGROUP_SUBSYS_COUNT));
#endif
#ifdef DNSK_CGROUP_SUBSYS_LAYOUT
MODULE_INFO(dnsk_cgroup_subsys_layout, __stringify(DNSK_CGROUP_SUBSYS_LAYOUT));
#endif
#ifdef DNSK_SEQ_FILE_PRIVATE
MODULE_INFO(dnsk_seq_file_private, __stringify(DNSK_SEQ_FILE_PRIVATE));
#endif

#endif /* _DNSK_ABI_H */
