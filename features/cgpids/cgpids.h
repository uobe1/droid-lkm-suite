/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_CGPIDS_H
#define _DNSK_CGPIDS_H

#include <linux/atomic.h>
#include <linux/cgroup.h>
#include <linux/rwlock.h>
#include <linux/kernfs.h>
#include <linux/rcupdate.h>
#include <linux/threads.h>
#include <linux/types.h>
#include <linux/xarray.h>

#define DNSK_PIDS_MAX (PID_MAX_LIMIT + 1ULL)

enum dnsk_pids_event {
	DNSK_PIDS_EVENT_MAX,
	DNSK_PIDS_EVENT_FORKFAIL,
	DNSK_PIDS_EVENT_COUNT,
};

/*
 * The rcu_head is deliberately FIRST.
 *
 * kfree_rcu() can only be used when the head sits at offset 0 (the kernel picks
 * a fast path from `offsetof(struct, member)`), and using it is what lets this
 * object be freed by kernel-resident code instead of by a callback living in
 * this module.  That is the difference between "the virtual controller pins the
 * module forever" and "the virtual controller can be unloaded": a queued
 * call_rcu() may run after module_exit, and it must not branch into freed text.
 */
struct dnsk_pids_state {
	struct rcu_head rcu;	/* MUST be first: kfree_rcu() contract */
	struct cgroup *cgrp;
	atomic64_t counter;
	atomic64_t limit;
	atomic64_t watermark;
	atomic64_t events[DNSK_PIDS_EVENT_COUNT];
	atomic64_t events_local[DNSK_PIDS_EVENT_COUNT];
	bool enabled;
	bool files_added;
};

extern struct xarray dnsk_pids_xa;
extern spinlock_t dnsk_pids_lock;
extern rwlock_t *dnsk_tasklist_lock;

/* Caller must hold rcu_read_lock(). */
struct dnsk_pids_state *dnsk_pids_lookup(struct cgroup *cgrp);
/* Sleepable; returns an existing or newly allocated state. */
struct dnsk_pids_state *dnsk_pids_ensure(struct cgroup *cgrp);
void dnsk_pids_destroy(struct cgroup *cgrp);
void dnsk_pids_activate(struct dnsk_pids_state *st);
void dnsk_pids_deactivate(struct dnsk_pids_state *st);

int dnsk_pids_try_charge(struct cgroup *cgrp, int num);
void dnsk_pids_charge(struct cgroup *cgrp, int num);
void dnsk_pids_uncharge(struct cgroup *cgrp, int num);

extern struct cftype dnsk_pids_files[];
extern struct cgroup_subsys_state *(*dnsk_of_css)(struct kernfs_open_file *of);

extern struct mutex *dnsk_cgroup_mutex;
extern void *dnsk_cgroup_base_files;
extern void *dnsk_cgrp_dfl_root;
extern void *dnsk_cgroup_kf_syscall_ops;
extern int (*dnsk_cgroup_mkdir)(struct kernfs_node *parent, const char *name,
			       umode_t mode);
extern int (*dnsk_cgroup_rmdir)(struct kernfs_node *kn);
extern int (*dnsk_cgroup_init_cftypes)(struct cgroup_subsys *ss,
				      struct cftype *cfts);
extern int (*dnsk_cgroup_addrm_files)(struct cgroup_subsys_state *css,
				     struct cgroup *cgrp,
				     struct cftype cfts[], bool is_add);
extern void (*dnsk_kernfs_activate)(struct kernfs_node *kn);

int dnsk_cgpids_files_init(void);
void dnsk_cgpids_files_exit(void);
void dnsk_cgpids_install_sysops(void);
void dnsk_cgpids_restore_sysops(void);

int dnsk_cgpids_migration_init(void);
void dnsk_cgpids_migration_exit(void);

struct dnsk_core_api;
struct dnsk_feature_desc;

/* O side: a real cgroup_subsys injected into the profile slot. */
int dnsk_cgpids_native_init(struct dnsk_core_api *api);

/* V side: the module-owned controller.  Compiled only into the virtual build. */
int dnsk_cgpids_virtual_init(struct dnsk_core_api *api,
			     const struct dnsk_feature_desc *desc);
void dnsk_cgpids_virtual_exit(struct dnsk_core_api *api);
int dnsk_cgpids_native_exit(void);
bool dnsk_cgpids_native_active(void);
/*
 * Hand the slot back and drop the module's self reference so rmmod can proceed.
 * Required before unloading because this kernel's cgroup_subsys has no owner
 * field, so the module must hold its own reference while it is live.
 */
int dnsk_cgpids_native_quiesce(void);

#endif /* _DNSK_CGPIDS_H */
