/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DNSK_KSUD_CORE_H
#define DNSK_KSUD_CORE_H

#include <linux/types.h>

int safe_read(void *dst, const void *src, size_t sz);
unsigned long kallsyms_name_to_addr(const char *name);
unsigned long module_kallsyms_lookup_name(const char *name);
extern unsigned int klnum_val;
void find_kallsyms_base(void);

#endif /* DNSK_KSUD_CORE_H */
