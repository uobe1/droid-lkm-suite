/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_CORE_H
#define _DNSK_CORE_H

#include <linux/types.h>

#define DNSK_CORE_MAGIC 0x444E534BU
#define DNSK_CORE_ABI 2

struct module;

#define DNSK_FEATURE_NAME_LEN 32
#define DNSK_HOOK_KEY_LEN 48

enum dnsk_status {
	DNSK_STATUS_UNKNOWN = 0,
	DNSK_STATUS_NOT_SHIPPED,
	DNSK_STATUS_FEASIBILITY,
	DNSK_STATUS_DEVELOPMENT,
	DNSK_STATUS_NATIVE,
	DNSK_STATUS_REAL,
	DNSK_STATUS_DIFFERENTIAL,
	DNSK_STATUS_VIRTUAL,
	DNSK_STATUS_NA,
	DNSK_STATUS_UNSUPPORTED,
};

enum dnsk_hook_type {
	DNSK_HOOK_KPROBE = 1,
	DNSK_HOOK_SYSCALL,
	DNSK_HOOK_DATA,
};

struct dnsk_hook_req {
	const char *key;
	enum dnsk_hook_type type;
	/* Registering module, for diagnostics.  Core does not pin it; the
	 * owner must remove every hook in module_exit(). */
	struct module *owner;
	const char *symbol;
	unsigned int syscall_nr;
	void *target;
	void *replacement;
	void **original_out;
};

struct dnsk_feature_desc {
	const char *name;
	const char *version;
	enum dnsk_status status;
	const char *caps;
	/* Registering module, for diagnostics.  Core does not pin it; the
	 * owner must remove its registration in module_exit(). */
	struct module *owner;
};

struct dnsk_core_api {
	u32 magic;
	u16 abi;
	u16 size;
	void *(*resolve)(const char *name);
	int (*hook_add)(const struct dnsk_hook_req *req);
	int (*hook_del)(const char *key);
	int (*feature_add)(const struct dnsk_feature_desc *desc);
	int (*feature_del)(const char *name);
	void (*log)(int level, const char *fmt, ...);
	/*
	 * dnsk_core_get() returns with one reference taken on this module.
	 * The caller must keep it for as long as the API may be used and
	 * release it with module_put(owner); the API must not be touched
	 * after that final put.
	 */
	struct module *owner;
};

struct dnsk_core_api *dnsk_core_get(u16 abi, u16 size);

#endif /* _DNSK_CORE_H */
