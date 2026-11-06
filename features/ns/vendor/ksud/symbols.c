// SPDX-License-Identifier: GPL-2.0-only
/*
 * Symbol access through the KernelSU module loader.  ksud resolves the
 * unexported imports against kallsyms before handing the module to the
 * kernel, so no in-module kallsyms scanner is needed.
 */

#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include "core.h"
#include "hint.h"

unsigned int klnum_val = 1;

int safe_read(void *dst, const void *src, size_t sz)
{
	return copy_from_kernel_nofault(dst, src, sz);
}

unsigned long kallsyms_name_to_addr(const char *name)
{
	return kallsyms_lookup_name(name);
}

void find_kallsyms_base(void)
{
}

int kallrecon_supply(const struct kallrecon_hint *hint)
{
	return 0;
}

enum kallrecon_fail kallrecon_fail_reason(void)
{
	return KALLRECON_OK;
}
