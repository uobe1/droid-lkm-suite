// SPDX-License-Identifier: GPL-2.0-only
/*
 * Safe kernel text/data patching for STRICT_KERNEL_RWX.
 *
 * Adapted from susfs4ksu-lkm kernel/patch_memory.c (GPL-2.0-only),
 * itself ported from KernelSU/SukiSU.  Writes go through a fixmap mapping of
 * the physical page, inside stop_machine(), via copy_to_kernel_nofault().
 */

#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/processor.h>
#include <linux/stop_machine.h>
#include <linux/uaccess.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/fixmap.h>
#include <asm-generic/fixmap.h>
#include "dnsk_internal.h"

atomic_t dnsk_patch_state = ATOMIC_INIT(-EOPNOTSUPP);

typedef void (*dnsk_set_fixmap_fn_t)(enum fixed_addresses idx, phys_addr_t phys,
				     pgprot_t prot);
typedef int (*dnsk_copy_nofault_fn_t)(void *dst, const void *src, size_t size);

static struct mm_struct *dnsk_init_mm;
static dnsk_set_fixmap_fn_t dnsk_set_fixmap_fn;
static dnsk_copy_nofault_fn_t dnsk_copy_nofault_fn;
static void (*dnsk_dcache_fn)(unsigned long start, unsigned long end);
static void (*dnsk_dcache_old_fn)(void *addr, size_t size);
static void (*dnsk_icache_fn)(unsigned long start, unsigned long end);

static unsigned long dnsk_phys_from_virt(unsigned long addr, int *err)
{
	struct mm_struct *mm = dnsk_init_mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	*err = -ENOENT;
	if (!mm)
		return 0;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;
#if defined(p4d_leaf)
	if (p4d_leaf(*p4d)) {
		*err = 0;
		return __p4d_to_phys(*p4d) + (addr & ~P4D_MASK);
	}
#endif

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return 0;
#if defined(pud_leaf)
	if (pud_leaf(*pud)) {
		*err = 0;
		return __pud_to_phys(*pud) + (addr & ~PUD_MASK);
	}
#endif

	pmd = pmd_offset(pud, addr);
#if defined(pmd_leaf)
	if (pmd_leaf(*pmd)) {
		*err = 0;
		return __pmd_to_phys(*pmd) + (addr & ~PMD_MASK);
	}
#endif
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return 0;

	pte = pte_offset_kernel(pmd, addr);
	if (!pte || !pte_present(*pte))
		return 0;

	*err = 0;
	return __pte_to_phys(*pte) + (addr & ~PAGE_MASK);
}

int dnsk_patch_init(void)
{
	void *mm = dnsk_resolve_symbol("init_mm");

	dnsk_init_mm = mm;
	/* GKI does not export these two, so they cannot be ELF imports. */
	dnsk_set_fixmap_fn = dnsk_resolve_symbol("__set_fixmap");
	dnsk_copy_nofault_fn = dnsk_resolve_symbol("copy_to_kernel_nofault");
	dnsk_dcache_fn = dnsk_resolve_symbol("dcache_clean_inval_poc");
	dnsk_dcache_old_fn = dnsk_resolve_symbol("__flush_dcache_area");
	dnsk_icache_fn = dnsk_resolve_symbol("caches_clean_inval_pou");
	if (!dnsk_icache_fn)
		dnsk_icache_fn = dnsk_resolve_symbol("__flush_icache_range");

	if (!dnsk_init_mm || !dnsk_set_fixmap_fn || !dnsk_copy_nofault_fn ||
	    (!dnsk_dcache_fn && !dnsk_dcache_old_fn)) {
		atomic_set(&dnsk_patch_state, -EOPNOTSUPP);
		dnsk_warn("patching disabled: init_mm=%px fixmap=%px copy=%px dcache=%px/%px\n",
			  dnsk_init_mm, dnsk_set_fixmap_fn, dnsk_copy_nofault_fn,
			  dnsk_dcache_fn, dnsk_dcache_old_fn);
		return -EOPNOTSUPP;
	}

	atomic_set(&dnsk_patch_state, 0);
	return 0;
}

bool dnsk_patch_ready(void)
{
	return atomic_read(&dnsk_patch_state) == 0;
}

static __nocfi noinline void dnsk_flush_dcache(unsigned long start, size_t len)
{
	if (dnsk_dcache_fn)
		dnsk_dcache_fn(start, start + len);
	else if (dnsk_dcache_old_fn)
		dnsk_dcache_old_fn((void *)start, len);
}

static __nocfi noinline void dnsk_flush_icache(unsigned long start, size_t len)
{
	if (dnsk_icache_fn)
		dnsk_icache_fn(start, start + len);
}

static __nocfi noinline int dnsk_fixmap_write(unsigned long phy,
					      const void *src, size_t len)
{
	void *map;
	int err;

	dnsk_set_fixmap_fn(FIX_TEXT_POKE0, phy, PAGE_KERNEL);
	map = (void *)(__fix_to_virt(FIX_TEXT_POKE0) + (phy & ~PAGE_MASK));
	err = dnsk_copy_nofault_fn(map, src, len);
	dnsk_set_fixmap_fn(FIX_TEXT_POKE0, 0, __pgprot(0));
	return err;
}

struct dnsk_patch_info {
	void *dst;
	const void *src;
	size_t len;
	atomic_t cpu_count;
	bool flush_icache;
	int ret;
};

static int dnsk_patch_nosync(void *dst, const void *src, size_t len,
			     bool flush_icache)
{
	unsigned long phy;
	int err;

	phy = dnsk_phys_from_virt((unsigned long)dst, &err);
	if (err)
		return err;

	err = dnsk_fixmap_write(phy, src, len);
	if (err)
		return err;

	if (flush_icache)
		dnsk_flush_icache((unsigned long)dst, len);
	dnsk_flush_dcache((unsigned long)dst, len);
	return 0;
}

static int dnsk_patch_cb(void *arg)
{
	struct dnsk_patch_info *info = arg;

	if (atomic_inc_return(&info->cpu_count) == num_online_cpus()) {
		info->ret = dnsk_patch_nosync(info->dst, info->src, info->len,
					      info->flush_icache);
		atomic_inc(&info->cpu_count);
	} else {
		while (atomic_read(&info->cpu_count) <= num_online_cpus())
			cpu_relax();
		isb();
	}
	return info->ret;
}

int dnsk_patch_text(void *dst, const void *src, size_t len, bool flush_icache)
{
	struct dnsk_patch_info info = {
		.dst = dst,
		.src = src,
		.len = len,
		.cpu_count = ATOMIC_INIT(0),
		.flush_icache = flush_icache,
		.ret = -EINVAL,
	};

	if (!dnsk_patch_ready())
		return -EOPNOTSUPP;
	if (!dst || !src || !len)
		return -EINVAL;

	return stop_machine(dnsk_patch_cb, &info, cpu_online_mask);
}

int dnsk_patch_pointer(void *target, void *value)
{
	unsigned long v = (unsigned long)value;

	if (!target)
		return -EINVAL;
	return dnsk_patch_text(target, &v, sizeof(v), false);
}
