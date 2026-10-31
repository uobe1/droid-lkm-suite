// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime symbol lookup without importing kallsyms_* symbols.
 *
 * kallsyms_lookup_name() is not exported on GKI.  Bootstrap it through a
 * temporary kprobe; fall back to kallsyms_on_each_symbol() for exact-name
 * lookups, including data symbols that cannot be probed.
 */

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/version.h>
#include "dnsk_internal.h"

struct dnsk_lookup_ctx {
	const char *name;
	unsigned long addr;
	int found;
};

static unsigned long (*kallsyms_lookup_name_fn)(const char *name);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
typedef int (*dnsk_walk_fn)(int (*fn)(void *, const char *, unsigned long),
			    void *data);
static int dnsk_walk_cb(void *data, const char *name, unsigned long addr)
#else
typedef int (*dnsk_walk_fn)(int (*fn)(void *, const char *, struct module *,
				      unsigned long), void *data);
static int dnsk_walk_cb(void *data, const char *name, struct module *mod,
			unsigned long addr)
#endif
{
	struct dnsk_lookup_ctx *ctx = data;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	if (mod)
		return 0;
#endif
	if (!name || !addr || strcmp(name, ctx->name))
		return 0;
	ctx->addr = addr;
	ctx->found = 1;
	return 1;
}

static dnsk_walk_fn kallsyms_on_each_symbol_fn;

static void *dnsk_bootstrap(const char *name)
{
	struct kprobe kp = { };
	void *addr;

	kp.symbol_name = name;
	if (register_kprobe(&kp))
		return NULL;
	addr = (void *)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

int dnsk_symbols_init(void)
{
	kallsyms_lookup_name_fn = dnsk_bootstrap("kallsyms_lookup_name");
	kallsyms_on_each_symbol_fn = dnsk_bootstrap("kallsyms_on_each_symbol");

	if (!kallsyms_lookup_name_fn && !kallsyms_on_each_symbol_fn) {
		dnsk_err("no runtime kallsyms resolver available\n");
		return -ENOENT;
	}
	return 0;
}

void dnsk_symbols_exit(void)
{
	kallsyms_lookup_name_fn = NULL;
	kallsyms_on_each_symbol_fn = NULL;
}

void *dnsk_resolve_symbol_exact(const char *name)
{
	struct dnsk_lookup_ctx ctx = { .name = name };

	if (!name || !name[0])
		return NULL;

	if (kallsyms_lookup_name_fn) {
		unsigned long addr = kallsyms_lookup_name_fn(name);

		if (addr)
			return (void *)addr;
	}

	if (kallsyms_on_each_symbol_fn) {
		kallsyms_on_each_symbol_fn(dnsk_walk_cb, &ctx);
		if (ctx.found)
			return (void *)ctx.addr;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	{
		char cfi_name[128];
		int n = snprintf(cfi_name, sizeof(cfi_name), "%s.cfi_jt", name);

		if (n > 0 && n < sizeof(cfi_name)) {
			if (kallsyms_lookup_name_fn) {
				unsigned long addr = kallsyms_lookup_name_fn(cfi_name);

				if (addr)
					return (void *)addr;
			}
			ctx.name = cfi_name;
			ctx.found = 0;
			if (kallsyms_on_each_symbol_fn) {
				kallsyms_on_each_symbol_fn(dnsk_walk_cb, &ctx);
				if (ctx.found)
					return (void *)ctx.addr;
			}
		}
	}
#endif
	return NULL;
}

void *dnsk_resolve_symbol(const char *name)
{
	return dnsk_resolve_symbol_exact(name);
}
