// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_ns: " fmt
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include "ipc-backend.h"
#include "ipc-util.h"

static DEFINE_HASHTABLE(task_states, 8);
static DEFINE_SPINLOCK(task_states_lock);
atomic_t ds_ipc_task_live = ATOMIC_INIT(0);
bool ds_ipc_active;

atomic_t ds_ns_inflight = ATOMIC_INIT(0);
bool ds_ns_quiescing;
static DECLARE_WAIT_QUEUE_HEAD(ds_ns_drain_wait);

bool ds_ns_enter(void)
{
	if (READ_ONCE(ds_ns_quiescing))
		return false;
	if (!try_module_get(THIS_MODULE))
		return false;
	atomic_inc(&ds_ns_inflight);
	/* Re-check after publishing the increment: if quiesce flipped while we
	 * were between the first check and the increment, back out so its wait
	 * observes a settled zero instead of a transient dip. */
	smp_mb__after_atomic();
	if (READ_ONCE(ds_ns_quiescing)) {
		if (atomic_dec_and_test(&ds_ns_inflight))
			wake_up_all(&ds_ns_drain_wait);
		module_put(THIS_MODULE);
		return false;
	}
	return true;
}

void ds_ns_leave(void)
{
	if (atomic_dec_and_test(&ds_ns_inflight))
		wake_up_all(&ds_ns_drain_wait);
	module_put(THIS_MODULE);
}

void ds_ns_hold(void)
{
	atomic_inc(&ds_ns_inflight);
}

void ds_ns_unhold(void)
{
	if (atomic_dec_and_test(&ds_ns_inflight))
		wake_up_all(&ds_ns_drain_wait);
}

int ds_ns_drain(void)
{
	long ret = wait_event_timeout(ds_ns_drain_wait,
				      atomic_read(&ds_ns_inflight) == 0,
				      msecs_to_jiffies(10000));
	return ret ? 0 : -EBUSY;
}

int ds_ns_quiesce(void)
{
	int ret;

	/* Block new entries at every gate before taking the grace period, so no
	 * fresh caller can slip in between the flag and the drain wait. */
	WRITE_ONCE(ds_ns_quiescing, true);
	WRITE_ONCE(ds_ipc_quiescing, true);
	synchronize_rcu();
	ret = ds_ns_drain();
	return ret;
}

struct ds_ipc_task *ds_ipc_task_find(struct task_struct *task)
{
	struct ds_ipc_task *state;
	unsigned long flags;
	spin_lock_irqsave(&task_states_lock, flags);
	hash_for_each_possible(task_states, state, node, (unsigned long)task) {
		if (state->task == task) {
			spin_unlock_irqrestore(&task_states_lock, flags);
			return state;
		}
	}
	spin_unlock_irqrestore(&task_states_lock, flags);
	return NULL;
}

void ds_ipc_task_install(struct ds_ipc_task *state, struct task_struct *task)
{
	unsigned long flags;
	state->task = task;
	INIT_LIST_HEAD(&state->shm_clist);
	spin_lock_irqsave(&task_states_lock, flags);
	hash_add(task_states, &state->node, (unsigned long)task);
	atomic_inc(&ds_ipc_task_live);
	spin_unlock_irqrestore(&task_states_lock, flags);
}

struct ds_ipc_task *ds_ipc_task_get_current(void)
{
	struct ds_ipc_task *state = ds_ipc_task_find(current);
	if (state)
		return state;
	state = kzalloc(sizeof(*state), GFP_KERNEL_ACCOUNT);
	if (state)
		ds_ipc_task_install(state, current);
	return state;
}

void ds_ipc_task_release(struct task_struct *task)
{
	struct ds_ipc_task *state = ds_ipc_task_find(task);
	unsigned long flags;
	if (!state)
		return;
	WARN_ON(state->undo_list || !list_empty(&state->shm_clist));
	spin_lock_irqsave(&task_states_lock, flags);
	hash_del(&state->node);
	atomic_dec(&ds_ipc_task_live);
	spin_unlock_irqrestore(&task_states_lock, flags);
	kfree(state);
}

int ds_ipc_backend_init(void)
{
	int ret = msg_init_ns(&init_ipc_ns);
	if (ret)
		return ret;
	sem_init_ns(&init_ipc_ns);
	shm_init_ns(&init_ipc_ns);
	if (!setup_ipc_sysctls(&init_ipc_ns)) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = ds_mqueue_init();
	if (ret) {
		retire_ipc_sysctls(&init_ipc_ns);
		goto fail;
	}
	ret = ds_ipc_proc_init();
	if (ret) {
		ds_mqueue_exit();
		retire_ipc_sysctls(&init_ipc_ns);
		goto fail;
	}
	return 0;
fail:
	sem_exit_ns(&init_ipc_ns);
	msg_exit_ns(&init_ipc_ns);
	shm_exit_ns(&init_ipc_ns);
	return ret;
}

void ds_ipc_backend_exit(void)
{
	ds_ipc_proc_exit();
	ds_mqueue_exit();
	retire_ipc_sysctls(&init_ipc_ns);
	sem_exit_ns(&init_ipc_ns);
	msg_exit_ns(&init_ipc_ns);
	shm_exit_ns(&init_ipc_ns);
	rcu_barrier();
	/*
	 * This must run after ds_ns_quiesce(): new IPC entry is blocked and the
	 * in-flight counter has drained, so no task is executing module text
	 * right now.  Any remaining per-task state therefore belongs to an idle
	 * task; it is kmalloc'd (not module memory) and nothing looks it up
	 * again once the hooks are gone, so orphaning it is inert.  Report the
	 * count instead of WARNing on a condition that no longer means "in use".
	 */
	if (!READ_ONCE(ds_ns_quiescing))
		pr_err("ipc: backend exit without prior quiesce\n");
	if (atomic_read(&ds_ipc_task_live))
		pr_info("ipc: %d idle task state(s) orphaned on unload\n",
			atomic_read(&ds_ipc_task_live));
}
