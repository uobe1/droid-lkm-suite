/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DNSK_KSUD_CORE_H
#define DNSK_KSUD_CORE_H

#include <linux/types.h>

struct path;
struct rw_semaphore;

int safe_read(void *dst, const void *src, size_t sz);
unsigned long kallsyms_name_to_addr(const char *name);
unsigned long module_kallsyms_lookup_name(const char *name);
extern unsigned int klnum_val;
void find_kallsyms_base(void);

/* Optional kernel hooks: absent symbols mean the hook is not compiled in. */
int ds_opt_security_path_chroot(const struct path *path);
int ds_opt_security_path_chown(const struct path *path, kuid_t uid, kgid_t gid);
void ds_opt_android_vh_mmap_lock_free(struct rw_semaphore *lock);

#endif /* DNSK_KSUD_CORE_H */
