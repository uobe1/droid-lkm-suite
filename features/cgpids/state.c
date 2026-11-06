// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtual pids controller state.  Counters are hierarchical: a task is
 * charged at every ancestor that has state, which makes pids.current a
 * subtree count exactly like the native controller.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "cgpids.h"

DEFINE_XARRAY(dnsk_pids_xa);
DEFINE_SPINLOCK(dnsk_pids_lock);
rwlock_t *dnsk_tasklist_lock;
static bool dnsk_pids_pinned;

struct dnsk_pids_state *dnsk_pids_lookup(struct cgroup *cgrp)
{
	return xa_load(&dnsk_pids_xa, (unsigned long)cgrp);
}

static void dnsk_pids_free_rcu(struct rcu_head *rcu)
{
	struct dnsk_pids_state *st = container_of(rcu, struct dnsk_pids_state,
						  rcu);

	kfree(st);
}

static bool dnsk_pids_pin_module(void)
{
	unsigned long flags;
	bool ok;

	spin_lock_irqsave(&dnsk_pids_lock, flags);
	if (dnsk_pids_pinned) {
		ok = true;
	} else {
		ok = try_module_get(THIS_MODULE);
		if (ok)
			dnsk_pids_pinned = true;
	}
	spin_unlock_irqrestore(&dnsk_pids_lock, flags);
	return ok;
}

/*
 * Tasks that predate the controller were never charged by the fork hook.
 * Seed a new state with the current subtree population so their migration
 * and exit paths stay balanced instead of driving the counter negative.
 */
static s64 dnsk_pids_subtree_tasks(struct cgroup *cgrp)
{
	struct task_struct *g, *t;
	s64 nr = 0;

	read_lock(dnsk_tasklist_lock);
	rcu_read_lock();
	for_each_process_thread(g, t) {
		struct css_set *cset;
		struct cgroup_subsys_state *css;

		cset = rcu_dereference(t->cgroups);
		if (!cset)
			continue;
		for (css = &cset->dfl_cgrp->self; css; css = css->parent) {
			if (css->cgroup == cgrp) {
				nr++;
				break;
			}
		}
	}
	rcu_read_unlock();
	read_unlock(dnsk_tasklist_lock);
	return nr;
}

struct dnsk_pids_state *dnsk_pids_ensure(struct cgroup *cgrp)
{
	struct dnsk_pids_state *st, *old;
	unsigned long flags;
	s64 seeded;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return NULL;
	st->cgrp = cgrp;
	atomic64_set(&st->limit, DNSK_PIDS_MAX);
	if (!dnsk_pids_pin_module()) {
		kfree(st);
		return NULL;
	}

	spin_lock_irqsave(&dnsk_pids_lock, flags);
	rcu_read_lock();
	old = xa_load(&dnsk_pids_xa, (unsigned long)cgrp);
	rcu_read_unlock();
	if (old) {
		spin_unlock_irqrestore(&dnsk_pids_lock, flags);
		kfree(st);
		return old;
	}
	spin_unlock_irqrestore(&dnsk_pids_lock, flags);

	seeded = dnsk_pids_subtree_tasks(cgrp);
	atomic64_set(&st->counter, seeded);
	atomic64_set(&st->watermark, seeded);

	spin_lock_irqsave(&dnsk_pids_lock, flags);
	old = xa_load(&dnsk_pids_xa, (unsigned long)cgrp);
	if (old) {
		spin_unlock_irqrestore(&dnsk_pids_lock, flags);
		kfree(st);
		return old;
	}
	if (xa_store(&dnsk_pids_xa, (unsigned long)cgrp, st, GFP_ATOMIC)) {
		spin_unlock_irqrestore(&dnsk_pids_lock, flags);
		kfree(st);
		return NULL;
	}
	spin_unlock_irqrestore(&dnsk_pids_lock, flags);
	return st;
}

void dnsk_pids_destroy(struct cgroup *cgrp)
{
	struct dnsk_pids_state *st;
	unsigned long flags;

	spin_lock_irqsave(&dnsk_pids_lock, flags);
	st = xa_erase(&dnsk_pids_xa, (unsigned long)cgrp);
	spin_unlock_irqrestore(&dnsk_pids_lock, flags);
	if (!st)
		return;
	call_rcu(&st->rcu, dnsk_pids_free_rcu);
}

void dnsk_pids_activate(struct dnsk_pids_state *st)
{
	/* The module is pinned once on the first state creation. */
	(void)st;
}

void dnsk_pids_deactivate(struct dnsk_pids_state *st)
{
	/* Pinned until reboot; see README. */
	(void)st;
}

static void dnsk_pids_update_peak(struct dnsk_pids_state *st, s64 val)
{
	s64 old = atomic64_read(&st->watermark);

	while (val > old) {
		if (atomic64_cmpxchg(&st->watermark, old, val) == old)
			break;
		old = atomic64_read(&st->watermark);
	}
}

static void dnsk_pids_event_fail(struct cgroup *forking,
				 struct dnsk_pids_state *over)
{
	struct cgroup_subsys_state *css;
	struct dnsk_pids_state *st;

	rcu_read_lock();
	st = dnsk_pids_lookup(forking);
	if (st && atomic64_inc_return(&st->events_local[DNSK_PIDS_EVENT_FORKFAIL]) == 1)
		pr_info("cgroup: fork rejected by dnsk pids controller\n");
	rcu_read_unlock();

	atomic64_inc(&over->events_local[DNSK_PIDS_EVENT_MAX]);
	for (css = &over->cgrp->self; css; css = css->parent) {
		rcu_read_lock();
		st = dnsk_pids_lookup(css->cgroup);
		if (st)
			atomic64_inc(&st->events[DNSK_PIDS_EVENT_MAX]);
		rcu_read_unlock();
	}
}

int dnsk_pids_try_charge(struct cgroup *cgrp, int num)
{
	struct cgroup_subsys_state *css;
	struct dnsk_pids_state *st, *over = NULL;
	s64 limit, new;

	rcu_read_lock();
	for (css = &cgrp->self; css; css = css->parent) {
		st = dnsk_pids_lookup(css->cgroup);
		if (!st)
			continue;
		new = atomic64_add_return(num, &st->counter);
		limit = atomic64_read(&st->limit);
		if (new > limit) {
			over = st;
			break;
		}
		dnsk_pids_update_peak(st, new);
	}

	if (!over) {
		rcu_read_unlock();
		for (css = &cgrp->self; css; css = css->parent) {
			rcu_read_lock();
			st = dnsk_pids_lookup(css->cgroup);
			if (st)
				dnsk_pids_activate(st);
			rcu_read_unlock();
		}
		return 0;
	}

	/* Roll back every charge, including the one that exceeded the limit. */
	for (css = &cgrp->self; css; css = css->parent) {
		st = dnsk_pids_lookup(css->cgroup);
		if (st)
			atomic64_sub(num, &st->counter);
		if (st == over)
			break;
	}
	rcu_read_unlock();
	dnsk_pids_event_fail(cgrp, over);
	return -EAGAIN;
}

void dnsk_pids_charge(struct cgroup *cgrp, int num)
{
	struct cgroup_subsys_state *css;
	struct dnsk_pids_state *st;
	s64 new;

	rcu_read_lock();
	for (css = &cgrp->self; css; css = css->parent) {
		st = dnsk_pids_lookup(css->cgroup);
		if (!st)
			continue;
		new = atomic64_add_return(num, &st->counter);
		dnsk_pids_update_peak(st, new);
		dnsk_pids_activate(st);
	}
	rcu_read_unlock();
}

void dnsk_pids_uncharge(struct cgroup *cgrp, int num)
{
	struct cgroup_subsys_state *css;
	struct dnsk_pids_state *st;
	s64 new;

	rcu_read_lock();
	for (css = &cgrp->self; css; css = css->parent) {
		st = dnsk_pids_lookup(css->cgroup);
		if (!st)
			continue;
		new = atomic64_sub_return(num, &st->counter);
		if (new < 0) {
			atomic64_set(&st->counter, 0);
			pr_warn_once("dnsk_cgpids: negative counter corrected\n");
		}
		dnsk_pids_deactivate(st);
	}
	rcu_read_unlock();
}
