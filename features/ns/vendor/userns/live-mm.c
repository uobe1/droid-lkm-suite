// SPDX-License-Identifier: GPL-2.0-only
/* Preserve mm user namespace ownership around native initialization; retain
 * native teardown including the USER_NS reference missing from this kernel.
 */
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/user_namespace.h>
#include <linux/iommu.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <trace/hooks/sched.h>
#include "core.h"
#include "live.h"
extern struct kmem_cache *mm_cachep;
extern struct mm_struct *ds_userns_mm_alloc_original(void);
extern void ds_userns_wake_up_new_task_original(struct task_struct *p);

/*
 * The native mm_init() is inlined into mm_alloc()/dup_mm() on this build, so
 * an entry hook on mm_init is not reliable.  Take one USER_NS reference for
 * every mm created after this module loads and drop it in __mmdrop().  mms
 * that predate the module stay untracked and are never put.
 */
#define DS_MM_HASH_BITS 6
struct ds_mm_ref {
	struct hlist_node node;
	struct mm_struct *mm;
};
static struct hlist_head ds_mm_refs[1 << DS_MM_HASH_BITS];
static DEFINE_SPINLOCK(ds_mm_refs_lock);

/* Caller holds ds_mm_refs_lock. */
static bool ds_mm_find_locked(struct mm_struct *mm)
{
	struct ds_mm_ref *ref;

	hlist_for_each_entry(ref, &ds_mm_refs[hash_ptr(mm, DS_MM_HASH_BITS)],
			     node) {
		if (ref->mm == mm)
			return true;
	}
	return false;
}

/*
 * Tracking outcome.  "Already" must be distinguished from "no memory": an
 * already tracked mm still needs its user_ns repaired (the inlined !USER_NS
 * stub overwrote it) but must not take a second reference, because
 * __mmdrop() releases exactly one.
 */
enum ds_mm_track_result {
	DS_MM_TRACK_NEW = 0,
	DS_MM_TRACK_ALREADY = 1,
	DS_MM_TRACK_NOMEM = -1,
};

static enum ds_mm_track_result ds_mm_track(struct mm_struct *mm)
{
	struct ds_mm_ref *ref = kmalloc(sizeof(*ref), GFP_ATOMIC);
	unsigned long flags;
	enum ds_mm_track_result result = DS_MM_TRACK_ALREADY;

	if (!ref)
		return DS_MM_TRACK_NOMEM;
	ref->mm = mm;
	spin_lock_irqsave(&ds_mm_refs_lock, flags);
	if (!ds_mm_find_locked(mm)) {
		hlist_add_head(&ref->node, &ds_mm_refs[hash_ptr(mm, DS_MM_HASH_BITS)]);
		result = DS_MM_TRACK_NEW;
	}
	spin_unlock_irqrestore(&ds_mm_refs_lock, flags);
	if (result != DS_MM_TRACK_NEW)
		kfree(ref);
	return result;
}

static bool ds_mm_untrack(struct mm_struct *mm)
{
	struct ds_mm_ref *ref, *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&ds_mm_refs_lock, flags);
	hlist_for_each_entry(ref, &ds_mm_refs[hash_ptr(mm, DS_MM_HASH_BITS)],
			     node) {
		if (ref->mm == mm) {
			found = ref;
			hlist_del(&ref->node);
			break;
		}
	}
	spin_unlock_irqrestore(&ds_mm_refs_lock, flags);
	kfree(found);
	return found != NULL;
}

struct mm_struct *ds_userns_mm_alloc(void)
{
	struct mm_struct *mm = ds_userns_mm_alloc_original();

	/*
	 * mm_init() is inlined on this build, so the USER_NS reference for a new
	 * mm is taken here and released in __mmdrop().  No-memory means the mm
	 * stays untracked: leaving user_ns as the stub set it is consistent with
	 * never dropping a reference for it.
	 */
	if (mm && ds_mm_track(mm) == DS_MM_TRACK_NEW)
		mm->user_ns = get_user_ns(current_user_ns());
	return mm;
}

void ds_userns_wake_up_new_task(struct task_struct *p)
{
	struct user_namespace *ns = current->mm ? current->mm->user_ns :
						   current_user_ns();

	/*
	 * A fresh mm (fork without CLONE_VM) had its user_ns overwritten by
	 * the inlined !USER_NS get_user_ns() stub.  Restore the creator's
	 * namespace; take a reference only when this call is what starts
	 * tracking the mm, because __mmdrop() releases exactly one.
	 * The task is not runnable yet, so this cannot race with its exit.
	 */
	if (!p->mm || p->mm == current->mm)
		goto done;
	switch (ds_mm_track(p->mm)) {
	case DS_MM_TRACK_NEW:
		p->mm->user_ns = get_user_ns(ns);
		break;
	case DS_MM_TRACK_ALREADY:
		/*
		 * mm_alloc() already took the reference and set the namespace,
		 * so this mm is consistent.  Assigning here would either drop
		 * the existing reference or add an unbalanced one.
		 */
		break;
	case DS_MM_TRACK_NOMEM:
		break;
	}
done:
	ds_userns_wake_up_new_task_original(p);
}
#define free_mm(mm) kmem_cache_free(mm_cachep, (mm))
static inline void mm_free_pgd(struct mm_struct *mm)
{
	pgd_free(mm, mm->pgd);
}

static const char * const resident_page_types[] = {
    [MM_FILEPAGES] = "MM_FILEPAGES", [MM_ANONPAGES] = "MM_ANONPAGES",
    [MM_SWAPENTS] = "MM_SWAPENTS", [MM_SHMEMPAGES] = "MM_SHMEMPAGES",
};
static void check_mm(struct mm_struct *mm)
{
	int i;

	BUILD_BUG_ON_MSG(ARRAY_SIZE(resident_page_types) != NR_MM_COUNTERS,
			 "Please make sure 'struct resident_page_types[]' is updated as well");

	for (i = 0; i < NR_MM_COUNTERS; i++) {
		long x = percpu_counter_sum(&mm->rss_stat[i]);

		if (unlikely(x))
			pr_alert("BUG: Bad rss-counter state mm:%p type:%s val:%ld\n",
				 mm, resident_page_types[i], x);
	}

	if (mm_pgtables_bytes(mm))
		pr_alert("BUG: non-zero pgtables_bytes on freeing mm: %ld\n",
				mm_pgtables_bytes(mm));

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !defined(CONFIG_SPLIT_PMD_PTLOCKS)
	VM_BUG_ON_MM(mm->pmd_huge_pte, mm);
#endif
}

static void do_check_lazy_tlb(void *arg)
{
	struct mm_struct *mm = arg;

	WARN_ON_ONCE(current->active_mm == mm);
}

static void do_shoot_lazy_tlb(void *arg)
{
	struct mm_struct *mm = arg;

	if (current->active_mm == mm) {
		WARN_ON_ONCE(current->mm);
		current->active_mm = &init_mm;
		switch_mm(mm, &init_mm, current);
	}
}

static void cleanup_lazy_tlbs(struct mm_struct *mm)
{
	if (!IS_ENABLED(CONFIG_MMU_LAZY_TLB_SHOOTDOWN)) {
		/*
		 * In this case, lazy tlb mms are refounted and would not reach
		 * __mmdrop until all CPUs have switched away and mmdrop()ed.
		 */
		return;
	}

	/*
	 * Lazy mm shootdown does not refcount "lazy tlb mm" usage, rather it
	 * requires lazy mm users to switch to another mm when the refcount
	 * drops to zero, before the mm is freed. This requires IPIs here to
	 * switch kernel threads to init_mm.
	 *
	 * archs that use IPIs to flush TLBs can piggy-back that lazy tlb mm
	 * switch with the final userspace teardown TLB flush which leaves the
	 * mm lazy on this CPU but no others, reducing the need for additional
	 * IPIs here. There are cases where a final IPI is still required here,
	 * such as the final mmdrop being performed on a different CPU than the
	 * one exiting, or kernel threads using the mm when userspace exits.
	 *
	 * IPI overheads have not found to be expensive, but they could be
	 * reduced in a number of possible ways, for example (roughly
	 * increasing order of complexity):
	 * - The last lazy reference created by exit_mm() could instead switch
	 *   to init_mm, however it's probable this will run on the same CPU
	 *   immediately afterwards, so this may not reduce IPIs much.
	 * - A batch of mms requiring IPIs could be gathered and freed at once.
	 * - CPUs store active_mm where it can be remotely checked without a
	 *   lock, to filter out false-positives in the cpumask.
	 * - After mm_users or mm_count reaches zero, switching away from the
	 *   mm could clear mm_cpumask to reduce some IPIs, perhaps together
	 *   with some batching or delaying of the final IPIs.
	 * - A delayed freeing and RCU-like quiescing sequence based on mm
	 *   switching to avoid IPIs completely.
	 */
	on_each_cpu_mask(mm_cpumask(mm), do_shoot_lazy_tlb, (void *)mm, 1);
	if (IS_ENABLED(CONFIG_DEBUG_VM_SHOOT_LAZIES))
		on_each_cpu(do_check_lazy_tlb, (void *)mm, 1);
}

void ds_userns_mmdrop(struct mm_struct *mm)
{
	BUG_ON(mm == &init_mm);
	WARN_ON_ONCE(mm == current->mm);

	/* Ensure no CPUs are using this as their lazy tlb mm */
	cleanup_lazy_tlbs(mm);

	WARN_ON_ONCE(mm == current->active_mm);
	mm_free_pgd(mm);
	destroy_context(mm);
	mmu_notifier_subscriptions_destroy(mm);
	check_mm(mm);
	if (ds_mm_untrack(mm))
		put_user_ns(mm->user_ns);
	mm_pasid_drop(mm);
	mm_destroy_cid(mm);
	percpu_counter_destroy_many(mm->rss_stat, NR_MM_COUNTERS);
	ds_opt_android_vh_mmap_lock_free(&mm->mmap_lock);

	free_mm(mm);
}
