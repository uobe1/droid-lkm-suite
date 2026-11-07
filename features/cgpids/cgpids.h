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

struct dnsk_pids_state {
	struct cgroup *cgrp;
	atomic64_t counter;
	atomic64_t limit;
	atomic64_t watermark;
	atomic64_t events[DNSK_PIDS_EVENT_COUNT];
	atomic64_t events_local[DNSK_PIDS_EVENT_COUNT];
	bool enabled;
	bool files_added;
	struct rcu_head rcu;
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
int dnsk_cgpids_native_init(struct dnsk_core_api *api);
void dnsk_cgpids_native_exit(void);
bool dnsk_cgpids_native_active(void);

#endif /* _DNSK_CGPIDS_H */
