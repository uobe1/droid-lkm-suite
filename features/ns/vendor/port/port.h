/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_PORT_H
#define DS_PORT_H

#include <linux/atomic.h>

struct task_struct;
extern atomic_t ds_pid_live;
extern atomic_t ds_ipc_live;
extern bool ds_hooks_active;
void ds_set_proc_exit_connector_fn(void (*fn)(struct task_struct *));
int ds_memfd_init(void);
void ds_memfd_exit(void);
int ds_pid_init(void);
void ds_pid_exit(void);
void ds_ipc_flush(void);
int ds_port_selftest(void);
int ds_hooks_install(void);
int ds_hooks_prepare(void);
int ds_hooks_uninstall(void);

#endif
