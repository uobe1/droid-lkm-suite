/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_IPC_BACKEND_H
#define DS_IPC_BACKEND_H
#include <linux/hashtable.h>
#include <linux/ipc_namespace.h>
#include <linux/sched.h>

struct ds_ipc_task {
	struct hlist_node node;
	struct task_struct *task;
	struct sem_undo_list *undo_list;
	struct list_head shm_clist;
};
extern atomic_t ds_ipc_task_live;
extern bool ds_ipc_active;
extern bool ds_ipc_quiescing;

/*
 * Put the backend's initial IPC namespace into the kernel's own init_nsproxy
 * and into every nsproxy that exists when the hooks go live, so tasks created
 * before the module can be joined with pidfd setns(CLONE_NEWIPC).  The restore
 * side clears those pointers again and must run before the hooks are removed
 * (it raises ds_ipc_quiescing first).
 */
int ds_ipc_backfill_install(void);
void ds_ipc_backfill_restore(void);

/* Only the task itself (or its parent before wake_up_new_task) owns this state. */
struct ds_ipc_task *ds_ipc_task_find(struct task_struct *task);
struct ds_ipc_task *ds_ipc_task_get_current(void);
void ds_ipc_task_install(struct ds_ipc_task *state, struct task_struct *task);
void ds_ipc_task_release(struct task_struct *task);
int ds_sem_clone_prepare(struct ds_ipc_task *state);
void ds_sem_clone_abort(struct ds_ipc_task *state);
void ds_exit_sem(struct task_struct *task);
void ds_exit_shm(struct task_struct *task);
void ds_shm_destroy_orphaned(struct ipc_namespace *ns);
long ds_do_shmat(int shmid, char __user *addr, int flags,
		unsigned long *result, unsigned long shmlba);
int ds_ipc_backend_init(void);
void ds_ipc_backend_exit(void);
int ds_mqueue_init(void);
void ds_mqueue_exit(void);
int ds_ipc_proc_init(void);
void ds_ipc_proc_exit(void);

static inline struct ipc_namespace *ds_current_ipc_ns(void)
{
	struct ipc_namespace *ns = current->nsproxy->ipc_ns;
	return ns ? ns : &init_ipc_ns;
}
#endif
