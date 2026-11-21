/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Droid-Hook RW wrapper layer.
 *
 * This file owns the library lifecycle and the thin wrappers around the
 * injected backend's single-entry rw() primitive.  It deliberately contains
 * no allocation, no global state and no libc/kernel-only helpers, so the same
 * source compiles both as part of an LKM and as a host selftest.
 *
 * Chunking mirrors the core ABI 5 contract: one request carries at most 256
 * bytes and must not cross a page boundary.  4096 is the conservative chunk
 * boundary because it is <= both the 4K and 16K GKI page sizes.
 */

#include "dh.h"

/*
 * Opcode numbers mirror the core ABI 5 single-entry rw() contract.  dh.h
 * deliberately does not publish opcode/flag/lock_type constants, so the
 * runtime and this library must agree on these numeric values out of band.
 * This is reported to the lead as a contract gap; do not renumber here.
 */
#define DH_OP_READ	1
#define DH_OP_WRITE	2
#define DH_OP_XCHG	3
#define DH_OP_CAS	4
#define DH_OP_PROBE	5

#define DH_RW_MAX	256
#define DH_PAGE_SIZE	4096UL

/* Implemented in dh_txn.c; library-internal, intentionally absent from dh.h. */
void dh_txn_reset(struct dh_ctx *ctx);
int dh_txn_rollback_locked(struct dh_ctx *ctx);

static void dh_req_init(struct dh_rw_req *req)
{
	req->opcode = 0;
	req->flags = 0;
	req->lock_type = 0;
	req->addr = NULL;
	req->src = NULL;
	req->dst = NULL;
	req->lock = NULL;
	req->len = 0;
	req->value = 0;
	req->old = 0;
	req->ret = DH_OK;
}

/*
 * Largest chunk that fits both the 256-byte request cap and the remainder of
 * the current page.  Using unsigned long for the address avoids uintptr_t,
 * which is not guaranteed to exist in every kernel build.
 */
static size_t dh_chunk_size(const void *addr, size_t remaining)
{
	unsigned long a = (unsigned long)addr;
	size_t page_off = (size_t)(a & (DH_PAGE_SIZE - 1UL));
	size_t room = (size_t)DH_PAGE_SIZE - page_off;
	size_t chunk = remaining;

	if (chunk > DH_RW_MAX)
		chunk = DH_RW_MAX;
	if (chunk > room)
		chunk = room;
	return chunk;
}

static int dh_bytes_equal(const void *a, const void *b, size_t len)
{
	const unsigned char *x = a;
	const unsigned char *y = b;
	size_t i;

	for (i = 0; i < len; i++) {
		if (x[i] != y[i])
			return 0;
	}
	return 1;
}

static int dh_backend_mandatory(const struct dh_backend *be)
{
	/*
	 * alloc/free are the only optional primitives (see dh.h).  Every other
	 * function pointer must be present before a ctx may be bound.
	 */
	return be->rw && be->patch_pointer && be->text_write && be->ro_write &&
	       be->probe && be->icache_flush && be->tlb_flush &&
	       be->quiesce_begin && be->quiesce_end;
}

int dh_ctx_init(struct dh_ctx *ctx, struct dh_backend *be, const char *owner)
{
	size_t i;

	if (!ctx || !be || !owner)
		return DH_E_INVAL;
	if (be->abi != DH_ABI)
		return DH_E_INVAL;
	if (!dh_backend_mandatory(be))
		return DH_E_INVAL;

	ctx->be = be;
	ctx->initialized = 1;
	ctx->addr_arbitrate = NULL;

	/* Bounded manual copy: no strncpy/strscpy dependency in either context. */
	for (i = 0; i + 1 < DH_KEY_LEN && owner[i]; i++)
		ctx->owner[i] = owner[i];
	ctx->owner[i] = '\0';

	dh_txn_reset(ctx);
	return DH_OK;
}

int dh_ctx_fini(struct dh_ctx *ctx)
{
	struct dh_backend *be;
	int ret = DH_OK;

	if (!ctx || !ctx->initialized || !ctx->be)
		return DH_E_INVAL;
	be = ctx->be;

	/*
	 * dh.h documents DH_E_QUIESCE for "runtime could not quiesce".  The
	 * backend's quiesce_begin/end return void, so the only observable
	 * failure is that the runtime stopped supplying them after init.
	 * Refuse to release the ctx in that case; the caller must retry.
	 */
	if (!be->quiesce_begin || !be->quiesce_end)
		return DH_E_QUIESCE;

	be->quiesce_begin();
	if (ctx->txn.active)
		ret = dh_txn_rollback_locked(ctx);
	be->quiesce_end();

	if (ret != DH_OK)
		return ret;

	ctx->initialized = 0;
	ctx->be = NULL;
	return DH_OK;
}

int dh_read(struct dh_ctx *ctx, void *addr, void *dst, size_t len)
{
	unsigned char *d = dst;
	unsigned char *a = addr;
	size_t done = 0;

	if (!ctx || !ctx->initialized || !ctx->be || !addr || !dst || !len)
		return DH_E_INVAL;

	while (done < len) {
		struct dh_rw_req req;
		size_t chunk = dh_chunk_size(a, len - done);
		int ret;

		dh_req_init(&req);
		req.opcode = DH_OP_READ;
		req.addr = a;
		req.dst = d;
		req.len = chunk;
		ret = ctx->be->rw(&req);
		if (ret != DH_OK)
			return ret;

		a += chunk;
		d += chunk;
		done += chunk;
	}
	return DH_OK;
}

int dh_write(struct dh_ctx *ctx, void *addr, const void *src, size_t len)
{
	const unsigned char *s = src;
	unsigned char *a = addr;
	size_t done = 0;

	if (!ctx || !ctx->initialized || !ctx->be || !addr || !src || !len)
		return DH_E_INVAL;

	while (done < len) {
		struct dh_rw_req req;
		unsigned char verify[DH_RW_MAX];
		size_t chunk = dh_chunk_size(a, len - done);
		int ret;

		dh_req_init(&req);
		req.opcode = DH_OP_WRITE;
		req.addr = a;
		req.src = s;
		req.len = chunk;
		ret = ctx->be->rw(&req);
		if (ret != DH_OK)
			return ret;

		/* Read-back verification is mandatory, not best-effort. */
		ret = dh_read(ctx, a, verify, chunk);
		if (ret != DH_OK)
			return ret;
		if (!dh_bytes_equal(verify, s, chunk)) {
			if (ctx->txn.active)
				ctx->txn.failed = 1;
			return DH_E_VERIFY;
		}

		a += chunk;
		s += chunk;
		done += chunk;
	}
	return DH_OK;
}

int dh_xchg64(struct dh_ctx *ctx, void *addr, u64 value, u64 *old)
{
	struct dh_rw_req req;
	int ret;

	if (!ctx || !ctx->initialized || !ctx->be || !addr)
		return DH_E_INVAL;
	if ((unsigned long)addr & 7UL)
		return DH_E_INVAL;

	dh_req_init(&req);
	req.opcode = DH_OP_XCHG;
	req.addr = addr;
	req.value = value;
	req.len = 8;
	ret = ctx->be->rw(&req);
	if (ret == DH_OK && old)
		*old = req.old;
	return ret;
}

int dh_cas64(struct dh_ctx *ctx, void *addr, u64 expected, u64 value)
{
	struct dh_rw_req req;

	if (!ctx || !ctx->initialized || !ctx->be || !addr)
		return DH_E_INVAL;
	if ((unsigned long)addr & 7UL)
		return DH_E_INVAL;

	dh_req_init(&req);
	req.opcode = DH_OP_CAS;
	req.addr = addr;
	req.old = expected;
	req.value = value;
	req.len = 8;
	return ctx->be->rw(&req);
}

void *dh_probe(struct dh_ctx *ctx, const char *name)
{
	if (!ctx || !ctx->initialized || !ctx->be || !name)
		return NULL;
	return ctx->be->probe(name);
}
