/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host harness for the Droid-Hook trampoline and shadow layers.
 *
 * It provides the stubs for the entry points implemented by other teammates
 * (dh_insn_*, dh_txn_*, dh_read) plus a fake backend whose text_write copies
 * into a writable instruction buffer.  Compiled as a shared library and driven
 * by test_dh_hook.py through ctypes; no kernel headers, no real patching.
 */

#include "dh.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Internal test surface: defined in the two .c files under test, deliberately
 * not exposed in dh.h.  Declared here so -Werror=implicit-function-declaration
 * does not fire. */
void dh_hook_test_reset(void);
int dh_hook_test_registry_count(void);
int dh_hook_test_arbitrate_calls(void);
int dh_hook_test_live_tramps(void);
size_t dh_hook_test_relo_len(struct dh_hook *hook);
u32 dh_hook_test_relo_word(struct dh_hook *hook, size_t idx);
u32 dh_hook_test_jump_word(struct dh_hook *hook, size_t idx);
u32 dh_hook_test_origin_word(struct dh_hook *hook, size_t idx);
int dh_hook_chain_run(struct dh_hook *hook, void *fargs,
		      int (*origin)(void *fargs));
int dh_shadow_put(struct dh_ctx *ctx, struct dh_shadow_item *item);
void dh_shadow_test_reset(void);
u32 dh_shadow_test_generation(void);
int dh_shadow_test_live(void);

/* ---- fake text / backend state ---- */

#define FAKE_INSNS		8
static u32 g_text[FAKE_INSNS];
static int g_text_writes;
static int g_quiesce_depth;
static int g_quiesce_calls;
static int g_icache_calls;
static int g_txn_begin_calls;
static int g_txn_commit_calls;
static int g_txn_rollback_calls;
static int g_text_write_fail;
static int g_arbitrate_reject;

static void *g_arbitrate_last_addr;
static char g_arbitrate_last_key[DH_KEY_LEN];

/* Allocation/release trace.  The uninstall contract is "restore, then release
 * in reverse order of allocation, all inside one quiesce window"; none of that
 * is observable from return codes, so the backend records it.  Index i refers
 * to the i-th allocation made through this backend since the last reset. */
#define TRACE_MAX		32
static void *g_alloc_trace[TRACE_MAX];
static int g_alloc_n;
static int g_free_order[TRACE_MAX];	/* alloc index of the freed pointer */
static int g_free_quiesce[TRACE_MAX];	/* quiesce depth at free time */
static int g_free_text_writes[TRACE_MAX]; /* text writes done before free */
static int g_free_n;

/* ---- stub: other teammates' translation units ---- */

int dh_insn_gen_absolute_jump(u32 *out, void *target)
{
	uintptr_t a = (uintptr_t)target;
	int i;

	if (!out)
		return DH_E_INVAL;
	for (i = 0; i < 4; i++)
		out[i] = 0xD4000000U + (u32)i;	/* recognizable fake opcodes */
	/* Encode the target into the last two words so the test can verify the
	 * jump points at the replacement / the fifth instruction. */
	out[2] = (u32)(a & 0xFFFFFFFFU);
	out[3] = (u32)(a >> 32);
	return DH_OK;
}

int dh_insn_relocate(const u32 *src, u32 *dst, void *src_pc, void *dst_pc,
		     size_t nr)
{
	size_t i;

	(void)src_pc;
	(void)dst_pc;
	if (!src || !dst || nr == 0 || nr > 4)
		return DH_E_INVAL;
	for (i = 0; i < nr; i++)
		dst[i] = src[i] ^ 0xA5A5A5A5U;	/* visible relocation marker */
	return DH_OK;
}

int dh_scan_refs(struct dh_ctx *ctx, void *old_base, size_t old_len,
		 void *new_base, void **sites, size_t max_sites)
{
	(void)ctx; (void)old_base; (void)old_len; (void)new_base;
	(void)sites; (void)max_sites;
	return 0;
}

int dh_patch_text_batch(struct dh_ctx *ctx, void **addrs, u32 **insns,
			size_t *lens, size_t nr)
{
	(void)ctx; (void)addrs; (void)insns; (void)lens; (void)nr;
	return DH_OK;
}

/* ---- stub: transaction layer ---- */

int dh_txn_begin(struct dh_ctx *ctx)
{
	if (!ctx)
		return DH_E_INVAL;
	g_txn_begin_calls++;
	ctx->txn.active = 1;
	ctx->txn.n = 0;
	ctx->txn.failed = 0;
	return DH_OK;
}

int dh_txn_record(struct dh_ctx *ctx, void *addr, size_t len,
		  enum dh_route route)
{
	struct dh_undo_ent *e;

	if (!ctx || !ctx->txn.active || !addr || len == 0 || len > 256)
		return DH_E_INVAL;
	if (ctx->txn.n >= DH_UNDO_SLOTS)
		return DH_E_NOSPC;
	e = &ctx->txn.undo[ctx->txn.n++];
	e->addr = addr;
	e->len = len;
	e->route = route;
	e->used = 1;
	memcpy(e->orig, addr, len);
	return DH_OK;
}

int dh_txn_commit(struct dh_ctx *ctx)
{
	if (!ctx || !ctx->txn.active)
		return DH_E_INVAL;
	g_txn_commit_calls++;
	ctx->txn.active = 0;
	ctx->txn.n = 0;
	return DH_OK;
}

int dh_txn_rollback(struct dh_ctx *ctx)
{
	u32 i;

	if (!ctx)
		return DH_E_INVAL;
	g_txn_rollback_calls++;
	for (i = ctx->txn.n; i > 0; i--) {
		struct dh_undo_ent *e = &ctx->txn.undo[i - 1];

		if (e->used && e->addr)
			memcpy(e->addr, e->orig, e->len);
	}
	ctx->txn.active = 0;
	ctx->txn.n = 0;
	ctx->txn.failed = 1;
	return DH_OK;
}

int dh_read(struct dh_ctx *ctx, void *addr, void *dst, size_t len)
{
	struct dh_rw_req req;

	if (!ctx || !ctx->be || !ctx->be->rw || !addr || !dst || !len)
		return DH_E_INVAL;
	memset(&req, 0, sizeof(req));
	req.addr = addr;
	req.dst = dst;
	req.len = len;
	req.opcode = 1;
	return ctx->be->rw(&req);
}

/* ---- fake backend ---- */

static int h_rw(struct dh_rw_req *req)
{
	if (!req || !req->addr || !req->dst || req->len == 0)
		return DH_E_INVAL;
	memcpy(req->dst, req->addr, req->len);
	req->ret = DH_OK;
	return DH_OK;
}

static int h_patch_pointer(void *target, void *value)
{
	if (!target)
		return DH_E_INVAL;
	memcpy(target, &value, sizeof(value));
	return DH_OK;
}

static int h_text_write(void *addr, const void *src, size_t len,
			enum dh_route route)
{
	(void)route;
	if (!addr || !src || len == 0)
		return DH_E_INVAL;
	if (g_text_write_fail)
		return DH_E_QUIESCE;
	memcpy(addr, src, len);
	g_text_writes++;
	return DH_OK;
}

static int h_ro_write(void *addr, const void *src, size_t len,
		      enum dh_route route)
{
	return h_text_write(addr, src, len, route);
}

static void *h_probe(const char *name)
{
	(void)name;
	return NULL;
}

static void *h_alloc(size_t size)
{
	void *p = malloc(size);

	if (p && g_alloc_n < TRACE_MAX)
		g_alloc_trace[g_alloc_n++] = p;
	return p;
}

static void h_free(const void *p)
{
	int i;

	if (g_free_n < TRACE_MAX) {
		g_free_order[g_free_n] = -1;
		for (i = 0; i < g_alloc_n; i++) {
			if (g_alloc_trace[i] == p) {
				g_free_order[g_free_n] = i;
				g_alloc_trace[i] = NULL;
				break;
			}
		}
		g_free_quiesce[g_free_n] = g_quiesce_depth;
		g_free_text_writes[g_free_n] = g_text_writes;
		g_free_n++;
	}
	free((void *)p);
}

static void h_icache_flush(void *addr, size_t len)
{
	(void)addr; (void)len;
	g_icache_calls++;
}

static void h_tlb_flush(void *addr, size_t len)
{
	(void)addr; (void)len;
}

static void h_quiesce_begin(void)
{
	g_quiesce_depth++;
	g_quiesce_calls++;
}

static void h_quiesce_end(void)
{
	if (g_quiesce_depth > 0)
		g_quiesce_depth--;
}

static int h_arbitrate(void *addr, const char *key)
{
	g_arbitrate_last_addr = addr;
	if (key)
		strncpy(g_arbitrate_last_key, key, DH_KEY_LEN - 1);
	return g_arbitrate_reject ? DH_E_BUSY : DH_OK;
}

static struct dh_backend g_be = {
	.rw = h_rw,
	.patch_pointer = h_patch_pointer,
	.text_write = h_text_write,
	.ro_write = h_ro_write,
	.probe = h_probe,
	.alloc = h_alloc,
	.free = h_free,
	.icache_flush = h_icache_flush,
	.tlb_flush = h_tlb_flush,
	.quiesce_begin = h_quiesce_begin,
	.quiesce_end = h_quiesce_end,
	.name = "host-harness",
	.abi = DH_ABI,
};

/* ---- exported harness surface (ctypes) ---- */

struct hook_result {
	int ret;
	struct dh_hook hook;
};

struct shadow_result {
	uintptr_t item;
	size_t size;
	u32 id;
	void *obj;
};

void harness_reset(int reject)
{
	size_t i;

	for (i = 0; i < FAKE_INSNS; i++)
		g_text[i] = 0x1000U + (u32)i;
	g_text_writes = 0;
	g_quiesce_depth = 0;
	g_quiesce_calls = 0;
	g_icache_calls = 0;
	g_txn_begin_calls = 0;
	g_txn_commit_calls = 0;
	g_txn_rollback_calls = 0;
	g_text_write_fail = 0;
	g_arbitrate_reject = reject;
	g_arbitrate_last_addr = NULL;
	memset(g_arbitrate_last_key, 0, sizeof(g_arbitrate_last_key));
	memset(g_alloc_trace, 0, sizeof(g_alloc_trace));
	g_alloc_n = 0;
	memset(g_free_order, 0, sizeof(g_free_order));
	memset(g_free_quiesce, 0, sizeof(g_free_quiesce));
	memset(g_free_text_writes, 0, sizeof(g_free_text_writes));
	g_free_n = 0;
	dh_hook_test_reset();
	dh_shadow_test_reset();
}

struct dh_ctx *harness_ctx(void)
{
	static struct dh_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.be = &g_be;
	ctx.initialized = 1;
	strncpy(ctx.owner, "harness", DH_KEY_LEN - 1);
	ctx.addr_arbitrate = h_arbitrate;
	return &ctx;
}

void *harness_text(void)
{
	return (void *)g_text;
}

struct hook_result harness_install(const char *key, void *replacement)
{
	struct hook_result r;

	memset(&r, 0, sizeof(r));
	r.ret = dh_hook_install(harness_ctx(), key, (void *)g_text,
				replacement, &r.hook);
	return r;
}

int harness_uninstall(struct dh_hook *hook)
{
	return dh_hook_uninstall(harness_ctx(), hook);
}

int harness_wrap(struct dh_hook *hook, int (*before)(void *),
		 int (*after)(void *))
{
	return dh_hook_wrap(harness_ctx(), hook, before, after);
}

int harness_chain_run(struct dh_hook *hook, void *fargs,
		      int (*origin)(void *))
{
	return dh_hook_chain_run(hook, fargs, origin);
}

struct shadow_result harness_shadow_get(void *obj, u32 id, size_t size)
{
	struct shadow_result r;
	struct dh_shadow_item *it;

	memset(&r, 0, sizeof(r));
	it = dh_shadow_get_or_alloc(harness_ctx(), obj, id, size);
	if (it) {
		r.item = (uintptr_t)it;
		r.size = it->size;
		r.id = it->id;
		r.obj = it->obj;
	}
	return r;
}

int harness_shadow_free(void *obj, u32 id)
{
	return dh_shadow_free(harness_ctx(), obj, id);
}

int harness_shadow_put(uintptr_t item)
{
	return dh_shadow_put(harness_ctx(), (struct dh_shadow_item *)item);
}

u32 harness_shadow_generation(void)
{
	return dh_shadow_test_generation();
}

int harness_shadow_live(void)
{
	return dh_shadow_test_live();
}

/* The flexible array is the payload callers care about.  These two helpers
 * let the test prove that a re-allocated key starts from zero instead of
 * relying on allocator addresses differing, which is not a contract: a bounded
 * pool legitimately reuses a cell (upstream kmalloc/kfree behaves the same). */
void harness_shadow_fill(uintptr_t item, u8 byte, size_t len)
{
	struct dh_shadow_item *it = (struct dh_shadow_item *)item;
	size_t i;

	if (!it)
		return;
	if (len > it->size)
		len = it->size;
	for (i = 0; i < len; i++)
		it->data[i] = byte;
}

int harness_shadow_dirty(uintptr_t item, size_t len)
{
	struct dh_shadow_item *it = (struct dh_shadow_item *)item;
	size_t i;

	if (!it)
		return -1;
	if (len > it->size)
		len = it->size;
	for (i = 0; i < len; i++)
		if (it->data[i])
			return 1;
	return 0;
}

u32 *harness_text_ptr(void)
{
	return g_text;
}

/* Instruction words are unsigned by nature.  Returning int here made
 * 0xD4000000 compare as a negative number in the ctypes test. */
uint32_t harness_text_word(size_t idx)
{
	return idx < FAKE_INSNS ? g_text[idx] : 0;
}

int harness_arbitrate_calls(void)
{
	return dh_hook_test_arbitrate_calls();
}

void *harness_arbitrate_addr(void)
{
	return g_arbitrate_last_addr;
}

int harness_registry_count(void)
{
	return dh_hook_test_registry_count();
}

int harness_live_tramps(void)
{
	return dh_hook_test_live_tramps();
}

size_t harness_relo_len(struct dh_hook *hook)
{
	return dh_hook_test_relo_len(hook);
}

u32 harness_relo_word(struct dh_hook *hook, size_t idx)
{
	return dh_hook_test_relo_word(hook, idx);
}

u32 harness_jump_word(struct dh_hook *hook, size_t idx)
{
	return dh_hook_test_jump_word(hook, idx);
}

u32 harness_origin_word(struct dh_hook *hook, size_t idx)
{
	return dh_hook_test_origin_word(hook, idx);
}

int harness_text_writes(void)
{
	return g_text_writes;
}

int harness_quiesce_calls(void)
{
	return g_quiesce_calls;
}

int harness_quiesce_depth(void)
{
	return g_quiesce_depth;
}

int harness_icache_calls(void)
{
	return g_icache_calls;
}

int harness_txn_rollbacks(void)
{
	return g_txn_rollback_calls;
}

void harness_fail_text_write(int on)
{
	g_text_write_fail = on;
}

/* ---- layout / trace introspection ---- */

/* The ctypes mirror must match the real struct byte for byte: a short mirror
 * would truncate the by-value return and turn every later field read into an
 * out-of-bounds access.  Asserting the size from C is the only way to prove
 * the Python side is not silently reading a partial object. */
size_t harness_hook_size(void)
{
	return sizeof(struct dh_hook);
}

size_t harness_hook_result_size(void)
{
	return sizeof(struct hook_result);
}

size_t harness_shadow_item_size(void)
{
	return sizeof(struct dh_shadow_item);
}

int harness_alloc_count(void)
{
	return g_alloc_n;
}

int harness_free_count(void)
{
	return g_free_n;
}

int harness_free_order(int i)
{
	return (i >= 0 && i < g_free_n) ? g_free_order[i] : -2;
}

int harness_free_quiesce(int i)
{
	return (i >= 0 && i < g_free_n) ? g_free_quiesce[i] : -1;
}

int harness_free_text_writes(int i)
{
	return (i >= 0 && i < g_free_n) ? g_free_text_writes[i] : -1;
}
