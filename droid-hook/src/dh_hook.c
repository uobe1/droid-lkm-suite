/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Droid-Hook function-level trampoline and before/after chain.
 *
 * Shape follows docs/evidence/droid-hook-design-research.md section 2.1: cover
 * the first four instructions of the target with an absolute jump, keep the
 * displaced instructions in a separate relo area that ends with a jump back to
 * the fifth instruction, and drive the chain with before-in-order /
 * after-in-reverse plus skip_origin.
 *
 * The library owns no global state through dh.h (struct dh_ctx has no registry
 * field), so the bounded install registry lives here.  It is deliberately
 * fixed-size: an unbounded registry would need an allocator in the unload
 * path, which the quiesce contract forbids.
 *
 * dh_insn_* and dh_txn_* are declared in dh.h and implemented in a separate
 * translation unit; this file only calls them.  dh_read() is likewise the
 * thin wrapper over the backend's rw primitive.
 */

#include "dh.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define DH_ORIGIN_INSNS		4
#define DH_JUMP_INSNS		4
#define DH_RELO_INSNS		32
#define DH_WRAP_MAX		16
#define DH_REG_MAX		16
#define DH_INSN_BYTES		(DH_ORIGIN_INSNS * (size_t)sizeof(u32))
#define DH_TRAMP_MAGIC		0x44485450U	/* "DHTP" */

struct dh_trampoline {
	u32 origin[DH_ORIGIN_INSNS];
	u32 jump[DH_JUMP_INSNS];
	u32 relo[DH_RELO_INSNS];
	size_t relo_len;
	void *target;
	void *replacement;
	u32 magic;
};

struct dh_wrap_item {
	int (*before)(void *fargs);
	int (*after)(void *fargs);
};

struct dh_chain {
	struct dh_wrap_item items[DH_WRAP_MAX];
	u32 nr;
};

struct dh_reg_ent {
	char key[DH_KEY_LEN];
	void *target;
	u8 used;
};

static struct dh_reg_ent dh_reg[DH_REG_MAX];
static struct dh_trampoline dh_tramp_pool[DH_REG_MAX];
static u8 dh_tramp_used[DH_REG_MAX];
static struct dh_chain dh_chain_pool[DH_REG_MAX];
static u8 dh_chain_used[DH_REG_MAX];
static int dh_arbitrate_calls;
static int dh_live_tramps;

static int dh_in_pool(const void *p, const void *base, size_t total)
{
	const unsigned char *a = (const unsigned char *)base;
	const unsigned char *x = (const unsigned char *)p;

	return x >= a && x < a + total;
}

static struct dh_trampoline *dh_tramp_alloc(struct dh_ctx *ctx)
{
	struct dh_trampoline *p;
	size_t i;

	if (ctx && ctx->be && ctx->be->alloc) {
		p = (struct dh_trampoline *)ctx->be->alloc(sizeof(*p));
		if (p) {
			memset(p, 0, sizeof(*p));
			dh_live_tramps++;
			return p;
		}
	}
	/* Fall back to the static pool so install never needs a sleeping
	 * allocator in the unload path. */
	for (i = 0; i < DH_REG_MAX; i++) {
		if (!dh_tramp_used[i]) {
			dh_tramp_used[i] = 1;
			p = &dh_tramp_pool[i];
			memset(p, 0, sizeof(*p));
			dh_live_tramps++;
			return p;
		}
	}
	return NULL;
}

static void dh_tramp_free(struct dh_ctx *ctx, void *p)
{
	size_t i;

	if (!p)
		return;
	if (dh_in_pool(p, dh_tramp_pool, sizeof(dh_tramp_pool))) {
		i = (size_t)(((const unsigned char *)p -
			      (const unsigned char *)dh_tramp_pool) /
			     sizeof(dh_tramp_pool[0]));
		if (i < DH_REG_MAX)
			dh_tramp_used[i] = 0;
	} else if (ctx && ctx->be && ctx->be->free) {
		ctx->be->free(p);
	}
	if (dh_live_tramps > 0)
		dh_live_tramps--;
}

static struct dh_chain *dh_chain_alloc(struct dh_ctx *ctx)
{
	struct dh_chain *p;
	size_t i;

	if (ctx && ctx->be && ctx->be->alloc) {
		p = (struct dh_chain *)ctx->be->alloc(sizeof(*p));
		if (p) {
			memset(p, 0, sizeof(*p));
			return p;
		}
	}
	for (i = 0; i < DH_REG_MAX; i++) {
		if (!dh_chain_used[i]) {
			dh_chain_used[i] = 1;
			p = &dh_chain_pool[i];
			memset(p, 0, sizeof(*p));
			return p;
		}
	}
	return NULL;
}

static void dh_chain_free(struct dh_ctx *ctx, void *p)
{
	size_t i;

	if (!p)
		return;
	if (dh_in_pool(p, dh_chain_pool, sizeof(dh_chain_pool))) {
		i = (size_t)(((const unsigned char *)p -
			      (const unsigned char *)dh_chain_pool) /
			     sizeof(dh_chain_pool[0]));
		if (i < DH_REG_MAX)
			dh_chain_used[i] = 0;
	} else if (ctx && ctx->be && ctx->be->free) {
		ctx->be->free(p);
	}
}

static int dh_reg_has_key(const char *key)
{
	size_t i;

	for (i = 0; i < DH_REG_MAX; i++)
		if (dh_reg[i].used && strncmp(dh_reg[i].key, key,
					      DH_KEY_LEN) == 0)
			return 1;
	return 0;
}

static int dh_reg_has_target(void *target)
{
	size_t i;

	for (i = 0; i < DH_REG_MAX; i++)
		if (dh_reg[i].used && dh_reg[i].target == target)
			return 1;
	return 0;
}

static int dh_reg_claim(const char *key, void *target)
{
	size_t i, klen = strlen(key);

	for (i = 0; i < DH_REG_MAX; i++) {
		if (dh_reg[i].used)
			continue;
		dh_reg[i].used = 1;
		dh_reg[i].target = target;
		memcpy(dh_reg[i].key, key, klen + 1);
		return 0;
	}
	return DH_E_NOSPC;
}

static void dh_reg_drop(const char *key, void *target)
{
	size_t i;

	for (i = 0; i < DH_REG_MAX; i++) {
		if (dh_reg[i].used && dh_reg[i].target == target &&
		    strncmp(dh_reg[i].key, key, DH_KEY_LEN) == 0) {
			memset(&dh_reg[i], 0, sizeof(dh_reg[i]));
			return;
		}
	}
}

/**
 * dh_hook_install() - install a function-level inline hook
 * @ctx: library context whose backend supplies text_write and rw
 * @key: stable unique name, shorter than DH_KEY_LEN
 * @target: first instruction of the function to hook
 * @replacement: replacement function reached by the entry jump
 * @out: filled on success with the installed hook description
 *
 * Install order: validate, claim the registry slot, arbitrate the address
 * with the legacy core hook registry, snapshot the four displaced
 * instructions, allocate trampoline and chain, build the entry jump and the
 * relo area (displaced instructions plus a jump back to instruction five),
 * then publish the entry jump through a transaction.  Any failure rolls the
 * transaction back, releases both allocations and leaves no registry entry.
 *
 * Return: %DH_OK, %DH_E_INVAL, %DH_E_BUSY, %DH_E_NOSPC, %DH_E_OPNOTSUPP,
 * or the error reported by the backend / transaction layer.
 */
int dh_hook_install(struct dh_ctx *ctx, const char *key, void *target,
		    void *replacement, struct dh_hook *out)
{
	struct dh_trampoline *t;
	struct dh_chain *c;
	u32 origin[DH_ORIGIN_INSNS];
	u32 back[DH_JUMP_INSNS];
	size_t klen;
	int ret;

	if (!ctx || !ctx->be || !key || !target || !replacement || !out)
		return DH_E_INVAL;
	klen = strlen(key);
	if (klen == 0 || klen >= DH_KEY_LEN)
		return DH_E_INVAL;
	if (!ctx->be->text_write)
		return DH_E_OPNOTSUPP;

	/* Shared key namespace with dnsk_core's hook registry: a duplicate
	 * key or a second writer at the same address is rejected. */
	if (dh_reg_has_key(key) || dh_reg_has_target(target))
		return DH_E_BUSY;
	dh_arbitrate_calls++;
	if (ctx->addr_arbitrate && ctx->addr_arbitrate(target, key))
		return DH_E_BUSY;

	ret = dh_read(ctx, target, origin, sizeof(origin));
	if (ret)
		return ret;

	t = dh_tramp_alloc(ctx);
	if (!t)
		return DH_E_NOSPC;
	c = dh_chain_alloc(ctx);
	if (!c) {
		dh_tramp_free(ctx, t);
		return DH_E_NOSPC;
	}

	memcpy(t->origin, origin, sizeof(origin));
	t->target = target;
	t->replacement = replacement;
	t->magic = DH_TRAMP_MAGIC;

	ret = dh_insn_gen_absolute_jump(t->jump, replacement);
	if (ret)
		goto fail;
	/* dh_insn_relocate() writes the displaced instructions into the relo
	 * area.  Its signature carries no output length, so the library
	 * reserves DH_ORIGIN_INSNS slots and appends the back-jump after
	 * them; see the contract note reported to the lead. */
	ret = dh_insn_relocate(origin, t->relo, target, (void *)t->relo,
			       DH_ORIGIN_INSNS);
	if (ret)
		goto fail;
	ret = dh_insn_gen_absolute_jump(back,
			(void *)((unsigned char *)target + DH_INSN_BYTES));
	if (ret)
		goto fail;
	memcpy(&t->relo[DH_ORIGIN_INSNS], back, sizeof(back));
	t->relo_len = DH_ORIGIN_INSNS + DH_JUMP_INSNS;

	ret = dh_txn_begin(ctx);
	if (ret)
		goto fail;
	ret = dh_txn_record(ctx, target, DH_INSN_BYTES, DH_ROUTE_ALIAS);
	if (ret)
		goto fail_txn;
	ret = ctx->be->text_write(target, t->jump, DH_INSN_BYTES,
				  DH_ROUTE_ALIAS);
	if (ret)
		goto fail_txn;
	if (ctx->be->icache_flush)
		ctx->be->icache_flush(target, DH_INSN_BYTES);
	ret = dh_txn_commit(ctx);
	if (ret)
		goto fail_txn;

	ret = dh_reg_claim(key, target);
	if (ret)
		goto fail_txn;

	memset(out, 0, sizeof(*out));
	memcpy(out->key, key, klen + 1);
	out->target = target;
	out->trampoline = t;
	out->chain = c;
	out->nr_chain = 0;
	out->installed = 1;
	return DH_OK;

fail_txn:
	dh_txn_rollback(ctx);
	dh_reg_drop(key, target);
fail:
	dh_chain_free(ctx, c);
	dh_tramp_free(ctx, t);
	return ret ? ret : DH_E_INVAL;
}

/**
 * dh_hook_uninstall() - restore a hook inside one quiesce window
 * @ctx: library context
 * @hook: hook previously returned by dh_hook_install()
 *
 * Reverse of install: restore the four original bytes, then release the
 * chain and trampoline, then drop the registry entry.  All of it happens
 * between be->quiesce_begin() and be->quiesce_end().  A DH_E_QUIESCE (or
 * any other error) aborts the unload: installed stays set, the trampoline
 * stays live, and the caller must retry instead of forcing the module out.
 *
 * Return: %DH_OK, %DH_E_INVAL, %DH_E_OPNOTSUPP, or %DH_E_QUIESCE.
 */
int dh_hook_uninstall(struct dh_ctx *ctx, struct dh_hook *hook)
{
	struct dh_trampoline *t;
	int ret;

	if (!ctx || !ctx->be || !hook || !hook->installed ||
	    !hook->trampoline || !hook->target)
		return DH_E_INVAL;
	t = (struct dh_trampoline *)hook->trampoline;
	if (t->magic != DH_TRAMP_MAGIC || t->target != hook->target)
		return DH_E_INVAL;
	if (!ctx->be->quiesce_begin || !ctx->be->quiesce_end ||
	    !ctx->be->text_write)
		return DH_E_OPNOTSUPP;

	ctx->be->quiesce_begin();
	ret = dh_txn_begin(ctx);
	if (ret)
		goto out_quiesce;
	ret = dh_txn_record(ctx, hook->target, DH_INSN_BYTES, DH_ROUTE_ALIAS);
	if (ret)
		goto out_rollback;
	ret = ctx->be->text_write(hook->target, t->origin, DH_INSN_BYTES,
				  DH_ROUTE_ALIAS);
	if (ret)
		goto out_rollback;
	if (ctx->be->icache_flush)
		ctx->be->icache_flush(hook->target, DH_INSN_BYTES);
	ret = dh_txn_commit(ctx);
	if (ret)
		goto out_rollback;

	/* Reverse order of install, all while quiesced. */
	hook->installed = 0;
	dh_reg_drop(hook->key, hook->target);
	dh_chain_free(ctx, hook->chain);
	hook->chain = NULL;
	hook->nr_chain = 0;
	dh_tramp_free(ctx, hook->trampoline);
	hook->trampoline = NULL;
	ctx->be->quiesce_end();
	return DH_OK;

out_rollback:
	dh_txn_rollback(ctx);
out_quiesce:
	ctx->be->quiesce_end();
	/* Never force-unload: keep the entry, the chain and the trampoline. */
	return ret ? ret : DH_E_QUIESCE;
}

/**
 * dh_hook_wrap() - append one before/after pair to the chain
 * @ctx: library context (unused, kept for signature symmetry)
 * @hook: installed hook
 * @before: called in registration order, non-zero means skip the origin
 * @after: called in reverse registration order
 *
 * The chain is bounded by DH_WRAP_MAX so no unbounded allocation is needed
 * on the hot path.  A NULL before or after is allowed for one-sided items.
 *
 * Return: %DH_OK, %DH_E_INVAL, or %DH_E_NOSPC.
 */
int dh_hook_wrap(struct dh_ctx *ctx, struct dh_hook *hook,
		 int (*before)(void *fargs), int (*after)(void *fargs))
{
	struct dh_chain *c;
	u32 i;

	(void)ctx;
	if (!hook || !hook->installed || !hook->chain)
		return DH_E_INVAL;
	if (!before && !after)
		return DH_E_INVAL;
	c = (struct dh_chain *)hook->chain;
	if (c->nr >= DH_WRAP_MAX)
		return DH_E_NOSPC;
	i = c->nr;
	c->items[i].before = before;
	c->items[i].after = after;
	c->nr = i + 1;
	hook->nr_chain = c->nr;
	return DH_OK;
}

/**
 * dh_hook_chain_run() - host-testable core of the chain transit path
 * @hook: installed hook
 * @fargs: opaque shared argument frame, passed unchanged to every callback
 * @origin: the original function stand-in, skipped when a before returns
 *          non-zero
 *
 * In the kernel build the transit entry is generated A64 code; this function
 * is the C reference for the same ordering rules and is what the host
 * selftest drives: before in order, origin unless skipped, after in reverse.
 *
 * Return: the first non-zero before result when skipped, else the origin
 * result, else %DH_OK.
 */
int dh_hook_chain_run(struct dh_hook *hook, void *fargs,
		      int (*origin)(void *fargs))
{
	struct dh_chain *c;
	u32 i;
	int ret, skip_val = 0;

	if (!hook || !hook->installed)
		return DH_E_INVAL;
	c = (struct dh_chain *)hook->chain;
	if (!c || c->nr == 0)
		return origin ? origin(fargs) : DH_OK;

	for (i = 0; i < c->nr; i++) {
		if (!c->items[i].before)
			continue;
		ret = c->items[i].before(fargs);
		if (ret)
			skip_val = ret;
	}
	if (skip_val)
		ret = skip_val;
	else
		ret = origin ? origin(fargs) : DH_OK;
	for (i = c->nr; i > 0; i--) {
		if (c->items[i - 1].after)
			(void)c->items[i - 1].after(fargs);
	}
	return ret;
}

/* ---- internal test surface (not part of dh.h) ---- */

void dh_hook_test_reset(void)
{
	memset(dh_reg, 0, sizeof(dh_reg));
	memset(dh_tramp_pool, 0, sizeof(dh_tramp_pool));
	memset(dh_tramp_used, 0, sizeof(dh_tramp_used));
	memset(dh_chain_pool, 0, sizeof(dh_chain_pool));
	memset(dh_chain_used, 0, sizeof(dh_chain_used));
	dh_arbitrate_calls = 0;
	dh_live_tramps = 0;
}

int dh_hook_test_registry_count(void)
{
	size_t i;
	int n = 0;

	for (i = 0; i < DH_REG_MAX; i++)
		if (dh_reg[i].used)
			n++;
	return n;
}

int dh_hook_test_arbitrate_calls(void)
{
	return dh_arbitrate_calls;
}

int dh_hook_test_live_tramps(void)
{
	return dh_live_tramps;
}

size_t dh_hook_test_relo_len(struct dh_hook *hook)
{
	struct dh_trampoline *t = (struct dh_trampoline *)hook->trampoline;

	if (!t || t->magic != DH_TRAMP_MAGIC)
		return 0;
	return t->relo_len;
}

u32 dh_hook_test_relo_word(struct dh_hook *hook, size_t idx)
{
	struct dh_trampoline *t = (struct dh_trampoline *)hook->trampoline;

	if (!t || t->magic != DH_TRAMP_MAGIC || idx >= DH_RELO_INSNS)
		return 0;
	return t->relo[idx];
}

u32 dh_hook_test_jump_word(struct dh_hook *hook, size_t idx)
{
	struct dh_trampoline *t = (struct dh_trampoline *)hook->trampoline;

	if (!t || t->magic != DH_TRAMP_MAGIC || idx >= DH_JUMP_INSNS)
		return 0;
	return t->jump[idx];
}

u32 dh_hook_test_origin_word(struct dh_hook *hook, size_t idx)
{
	struct dh_trampoline *t = (struct dh_trampoline *)hook->trampoline;

	if (!t || t->magic != DH_TRAMP_MAGIC || idx >= DH_ORIGIN_INSNS)
		return 0;
	return t->origin[idx];
}
