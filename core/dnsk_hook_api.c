// SPDX-License-Identifier: GPL-2.0-only
/*
 * Inline-hook control plane for dnsk_core, built on the droid-hook library.
 *
 * The library is compiled into dnsk_core.ko by path (see core/dnsk_dh_*.c and
 * Kbuild); this file only owns one struct dh_ctx, adapts the suite's existing
 * primitives to the struct dh_backend contract, and keeps the key -> dh_hook
 * table that hook_uninstall() and /proc/dnsk/status are keyed on.  Core stores
 * the hooks but does not pin the registering module: a feature must call
 * hook_uninstall() for every key it installed before its module_exit().
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include "dnsk_internal.h"
#include "dh.h"

#define DNSK_INLINE_MAX DNSK_MAX_HOOKS

struct dnsk_inline_slot {
	bool used;
	char key[DNSK_HOOK_KEY_LEN];
	struct dh_hook hook;
};

static struct dnsk_inline_slot dnsk_inline[DNSK_INLINE_MAX];
static DEFINE_MUTEX(dnsk_inline_lock);

static struct dh_ctx dnsk_dh_ctx;
static struct dh_backend dnsk_dh_be;
static bool dnsk_dh_ready;

static void *(*dnsk_module_alloc_fn)(unsigned long size);
static void (*dnsk_module_free_fn)(void *addr);

/*
 * Executable origin blocks are retired, never freed.  Restoring the entry only
 * stops new callers; an in-flight replacement can still be executing inside the
 * origin block, and this backend has no in-flight tracking that covers a task
 * preempted in the trampoline.  Freeing here would turn that into a
 * use-after-free of module memory.  The leak is a few tens of bytes per hook
 * per module load, chosen over a panic on unload.
 */
#define DNSK_RETIRED_MAX 64
static void *dnsk_retired[DNSK_RETIRED_MAX];
static unsigned int dnsk_retired_nr;
static unsigned int dnsk_retired_dropped;

static int dnsk_dh_ret(int ret)
{
	switch (ret) {
	case DH_OK:
		return 0;
	case DH_E_VERIFY:
		return -EIO;
	case DH_E_QUIESCE:
		return -EBUSY;
	default:
		return ret;
	}
}

/* struct dh_rw_req and struct dnsk_rw_req have the same field order and
 * numeric opcode/lock values; copy explicitly so the two headers can drift
 * independently without an aliasing cast. */
static int dnsk_dh_rw(struct dh_rw_req *req)
{
	struct dnsk_rw_req r;
	int ret;

	if (!req)
		return -EINVAL;
	memset(&r, 0, sizeof(r));
	r.opcode = req->opcode;
	r.flags = req->flags;
	r.lock_type = req->lock_type;
	r.addr = req->addr;
	r.src = req->src;
	r.dst = req->dst;
	r.lock = req->lock;
	r.len = req->len;
	r.value = req->value;
	r.old = req->old;
	ret = dnsk_rw_op(&r);
	req->old = r.old;
	req->ret = r.ret;
	return ret;
}

/*
 * Entry publication and rollback both write executable kernel text through the
 * fixmap + stop_machine path.  dnsk_patch_text() flushes icache and dcache
 * inside that one window, so flush_icache is always true here.  quiesce_begin
 * and quiesce_end are no-ops: the atomic window is the stop_machine inside
 * each text_write, not an independent barrier around the library call.
 */
static int dnsk_dh_text_write(void *addr, const void *src, size_t len,
			      enum dh_route route)
{
	(void)route;
	return dnsk_patch_text(addr, src, len, true);
}

static int dnsk_dh_ro_write(void *addr, const void *src, size_t len,
			    enum dh_route route)
{
	(void)route;
	return dnsk_patch_text(addr, src, len, true);
}

static void *dnsk_dh_alloc(size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

static void dnsk_dh_free(const void *p)
{
	kfree(p);
}

static void *dnsk_dh_alloc_exec(size_t size)
{
	if (!dnsk_module_alloc_fn)
		return NULL;
	return dnsk_module_alloc_fn((unsigned long)size);
}

static void dnsk_dh_free_exec(const void *p)
{
	if (!p)
		return;
	if (dnsk_retired_nr < ARRAY_SIZE(dnsk_retired))
		dnsk_retired[dnsk_retired_nr++] = (void *)p;
	else
		dnsk_retired_dropped++;
}

/* Fixmap writes never change a PTE mapping, so there is no TLB entry to
 * invalidate.  The field is mandatory in struct dh_backend, so expose a
 * documented no-op rather than leaving it NULL. */
static void dnsk_dh_tlb_flush(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

static void dnsk_dh_quiesce_begin(void)
{
}

static void dnsk_dh_quiesce_end(void)
{
}

static struct dnsk_inline_slot *dnsk_slot_find_locked(const char *key)
{
	int i;

	for (i = 0; i < DNSK_INLINE_MAX; i++) {
		if (dnsk_inline[i].used && !strcmp(dnsk_inline[i].key, key))
			return &dnsk_inline[i];
	}
	return NULL;
}

int dnsk_hook_install_internal(const struct dnsk_inline_hook_req *req)
{
	struct dnsk_inline_slot *slot = NULL;
	void *target;
	int i, ret;

	if (!req || !req->key || !req->key[0] ||
	    strlen(req->key) >= DNSK_HOOK_KEY_LEN)
		return -EINVAL;
	if (!req->symbol || !req->symbol[0] || !req->replacement || !req->owner)
		return -EINVAL;
	if (!dnsk_dh_ready)
		return -EOPNOTSUPP;

	mutex_lock(&dnsk_inline_lock);
	if (dnsk_slot_find_locked(req->key)) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < DNSK_INLINE_MAX; i++) {
		if (!dnsk_inline[i].used) {
			slot = &dnsk_inline[i];
			break;
		}
	}
	if (!slot) {
		ret = -ENOSPC;
		goto out;
	}

	target = dnsk_resolve_symbol(req->symbol);
	if (!target) {
		ret = -ENOENT;
		goto out;
	}

	memset(slot, 0, sizeof(*slot));
	/*
	 * The library writes *origin_out before it publishes the entry jump, so
	 * a caller that enters the new entry always sees a callable origin.  A
	 * request that needs an origin but has no executable allocator fails
	 * with -EOPNOTSUPP before any entry is written.
	 */
	ret = dnsk_dh_ret(dh_hook_install_origin(&dnsk_dh_ctx, req->key, target,
						 req->replacement, &slot->hook,
						 req->origin_out));
	if (ret)
		goto out;

	slot->used = true;
	strscpy(slot->key, req->key, sizeof(slot->key));
	ret = 0;
out:
	mutex_unlock(&dnsk_inline_lock);
	return ret;
}

int dnsk_hook_uninstall_internal(const char *key)
{
	struct dnsk_inline_slot *slot;
	int ret;

	if (!key)
		return -EINVAL;

	mutex_lock(&dnsk_inline_lock);
	slot = dnsk_slot_find_locked(key);
	if (!slot) {
		mutex_unlock(&dnsk_inline_lock);
		return -ENOENT;
	}
	ret = dnsk_dh_ret(dh_hook_uninstall(&dnsk_dh_ctx, &slot->hook));
	if (ret) {
		/* Keep the entry and the slot: the caller must not free handler
		 * data while the original entry could not be proven restored. */
		dnsk_err("hooks: failed to restore %s: %d\n", key, ret);
		mutex_unlock(&dnsk_inline_lock);
		return ret;
	}
	memset(slot, 0, sizeof(*slot));
	mutex_unlock(&dnsk_inline_lock);

	synchronize_rcu();
	return 0;
}

unsigned int dnsk_hook_count(void)
{
	unsigned int n = 0;
	int i;

	mutex_lock(&dnsk_inline_lock);
	for (i = 0; i < DNSK_INLINE_MAX; i++)
		if (dnsk_inline[i].used)
			n++;
	mutex_unlock(&dnsk_inline_lock);
	return n;
}

int dnsk_hooks_init(void)
{
	int ret;

	memset(dnsk_inline, 0, sizeof(dnsk_inline));
	dnsk_retired_nr = 0;
	dnsk_retired_dropped = 0;
	memset(&dnsk_dh_be, 0, sizeof(dnsk_dh_be));

	dnsk_dh_be.rw = dnsk_dh_rw;
	dnsk_dh_be.patch_pointer = dnsk_patch_pointer;
	dnsk_dh_be.text_write = dnsk_dh_text_write;
	dnsk_dh_be.ro_write = dnsk_dh_ro_write;
	dnsk_dh_be.probe = dnsk_resolve_symbol;
	dnsk_dh_be.alloc = dnsk_dh_alloc;
	dnsk_dh_be.free = dnsk_dh_free;
	dnsk_dh_be.icache_flush = dnsk_icache_flush;
	dnsk_dh_be.tlb_flush = dnsk_dh_tlb_flush;
	dnsk_dh_be.quiesce_begin = dnsk_dh_quiesce_begin;
	dnsk_dh_be.quiesce_end = dnsk_dh_quiesce_end;
	dnsk_dh_be.name = "dnsk_core";
	dnsk_dh_be.abi = DH_ABI;

	dnsk_module_alloc_fn = dnsk_resolve_symbol("module_alloc");
	dnsk_module_free_fn = dnsk_resolve_symbol("module_memfree");
	if (dnsk_module_alloc_fn && dnsk_module_free_fn) {
		dnsk_dh_be.alloc_exec = dnsk_dh_alloc_exec;
		dnsk_dh_be.free_exec = dnsk_dh_free_exec;
	} else {
		dnsk_dh_be.alloc_exec = NULL;
		dnsk_dh_be.free_exec = NULL;
		dnsk_warn("hooks: module_alloc/module_memfree missing; call-through origin disabled\n");
	}

	ret = dh_ctx_init(&dnsk_dh_ctx, &dnsk_dh_be, "dnsk_core");
	if (ret) {
		dnsk_err("hooks: droid-hook backend rejected: %d\n", ret);
		return -EINVAL;
	}
	dnsk_dh_ready = true;
	dnsk_info("hooks: droid-hook backend ready (module_alloc=%px module_memfree=%px origin=%s)\n",
		  dnsk_module_alloc_fn, dnsk_module_free_fn,
		  dnsk_dh_be.alloc_exec ? "enabled" : "disabled");
	return 0;
}

void dnsk_hooks_exit(void)
{
	int i;

	if (!dnsk_dh_ready)
		return;

	mutex_lock(&dnsk_inline_lock);
	for (i = 0; i < DNSK_INLINE_MAX; i++) {
		int ret;

		if (!dnsk_inline[i].used)
			continue;
		ret = dnsk_dh_ret(dh_hook_uninstall(&dnsk_dh_ctx,
						    &dnsk_inline[i].hook));
		if (ret) {
			dnsk_err("hooks: exit could not restore %s: %d\n",
				 dnsk_inline[i].key, ret);
			continue;
		}
		memset(&dnsk_inline[i], 0, sizeof(dnsk_inline[i]));
	}
	mutex_unlock(&dnsk_inline_lock);

	/* Match dnsk_hook_uninstall_internal(): a caller that entered just before
	 * the restore must leave before any handler data is released. */
	synchronize_rcu();
	dh_ctx_fini(&dnsk_dh_ctx);
	dnsk_dh_ready = false;
}
