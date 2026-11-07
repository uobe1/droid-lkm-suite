/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_INTERNAL_H
#define _DNSK_INTERNAL_H

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include "include/dnsk_core.h"

#define DNSK_MAX_HOOKS 64
#define DNSK_MAX_FEATURES 16
#define DNSK_PANIC_OP_LEN 48

#define dnsk_info(fmt, ...) pr_info("dnsk: " fmt, ##__VA_ARGS__)
#define dnsk_warn(fmt, ...) pr_warn("dnsk: " fmt, ##__VA_ARGS__)
#define dnsk_err(fmt, ...) pr_err("dnsk: " fmt, ##__VA_ARGS__)

struct dnsk_hook {
	char key[DNSK_HOOK_KEY_LEN];
	enum dnsk_hook_type type;
	bool active;
	union {
		struct {
			struct kprobe kp;
			const char *symbol;
			unsigned long addr;
			int (*pre_handler)(struct kprobe *, struct pt_regs *);
		} kprobe;
		struct {
			unsigned int nr;
			unsigned long *slot;
			unsigned long original;
		} syscall;
		struct {
			void *target;
			unsigned long original;
		} data;
		struct {
			struct kretprobe krp;
			const char *symbol;
			unsigned long addr;
			int (*entry_handler)(struct kretprobe_instance *,
					     struct pt_regs *);
			int (*ret_handler)(struct kretprobe_instance *,
					   struct pt_regs *);
		} kretprobe;
	} u;
};

struct dnsk_feature {
	char name[DNSK_FEATURE_NAME_LEN];
	char version[DNSK_FEATURE_NAME_LEN];
	char caps[128];
	enum dnsk_status status;
	bool active;
};

int dnsk_symbols_init(void);
void dnsk_symbols_exit(void);
void *dnsk_resolve_symbol(const char *name);
void *dnsk_resolve_symbol_exact(const char *name);
void dnsk_log(int level, const char *fmt, ...);

int dnsk_patch_init(void);
bool dnsk_patch_ready(void);
int dnsk_patch_text(void *dst, const void *src, size_t len, bool flush_icache);
int dnsk_patch_pointer(void *target, void *value);
int dnsk_virt_to_phys(void *va, unsigned long *pa);

int dnsk_hooks_init(void);
void dnsk_hooks_exit(void);
int dnsk_hook_add_internal(const struct dnsk_hook_req *req);
int dnsk_hook_del_internal(const char *key);
unsigned int dnsk_hook_count(void);

int dnsk_features_init(void);
void dnsk_features_exit(void);
int dnsk_feature_add_internal(const struct dnsk_feature_desc *desc);
int dnsk_feature_del_internal(const char *name);
unsigned int dnsk_feature_count(void);
void dnsk_features_show(struct seq_file *m);
const char *dnsk_status_name(enum dnsk_status status);

int dnsk_diag_init(void);
void dnsk_diag_exit(void);
struct proc_dir_entry;
struct proc_dir_entry *dnsk_proc_dir_entry(void);
int dnsk_rw_init(struct proc_dir_entry *proc_dir);
void dnsk_rw_exit(void);

int dnsk_panic_init(void);
void dnsk_panic_exit(void);
bool dnsk_panic_ready(void);
void dnsk_panic_mark(const char *op);

extern atomic_t dnsk_patch_state;

#endif /* _DNSK_INTERNAL_H */
