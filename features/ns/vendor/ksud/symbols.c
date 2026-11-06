// SPDX-License-Identifier: GPL-2.0-only
/*
 * Symbol access through the KernelSU module loader.  ksud resolves the
 * unexported imports against kallsyms before handing the module to the
 * kernel, so no in-module kallsyms scanner is needed.
 */

#include <linux/kallsyms.h>
#include <linux/path.h>
#include <linux/rwsem.h>
#include <linux/uidgid.h>
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

int ds_opt_security_path_chroot(const struct path *path)
{
	static int (*fn)(const struct path *);

	if (!fn)
		fn = (void *)kallsyms_name_to_addr("security_path_chroot");
	return fn ? fn(path) : 0;
}

int ds_opt_security_path_chown(const struct path *path, kuid_t uid, kgid_t gid)
{
	static int (*fn)(const struct path *, kuid_t, kgid_t);

	if (!fn)
		fn = (void *)kallsyms_name_to_addr("security_path_chown");
	return fn ? fn(path, uid, gid) : 0;
}

void ds_opt_android_vh_mmap_lock_free(struct rw_semaphore *lock)
{
	static void (*fn)(void *, struct rw_semaphore *);

	if (!fn)
		fn = (void *)kallsyms_name_to_addr(
			"__traceiter_android_vh_mmap_lock_free");
	if (fn)
		fn(NULL, lock);
}
