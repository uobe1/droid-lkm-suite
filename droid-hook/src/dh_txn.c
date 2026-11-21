/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Droid-Hook transaction and rollback layer.
 *
 * The undo log is a fixed-size array embedded in struct dh_txn; this file
 * never allocates, so a rollback on a panic path is safe.  Every entry stores
 * the original bytes, not a description of the mutation, and rollback is a
 * plain reverse-order replay inside one quiesce window.
 *
 * dh.h deliberately omits an internal header for library-private entry
 * points, so the two prototypes shared with dh_rw.c are repeated here with
 * identical signatures.
 */

#include "dh.h"

/* Library-internal, implemented here and called from dh_rw.c / this file. */
void dh_txn_reset(struct dh_ctx *ctx);
int dh_txn_rollback_locked(struct dh_ctx *ctx);

static int dh_route_valid(enum dh_route route)
{
	return route == DH_ROUTE_ALIAS || route == DH_ROUTE_PTE ||
	       route == DH_ROUTE_FIXMAP || route == DH_ROUTE_DMA;
}

static int dh_undo_bytes_equal(const unsigned char *a,
			       const unsigned char *b, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

void dh_txn_reset(struct dh_ctx *ctx)
{
	size_t i, j;

	if (!ctx)
		return;

	ctx->txn.ctx = ctx;
	ctx->txn.n = 0;
	ctx->txn.active = 0;
	ctx->txn.failed = 0;
	for (i = 0; i < (size_t)DH_UNDO_SLOTS; i++) {
		ctx->txn.undo[i].addr = NULL;
		ctx->txn.undo[i].len = 0;
		ctx->txn.undo[i].route = DH_ROUTE_ALIAS;
		ctx->txn.undo[i].used = 0;
		for (j = 0; j < sizeof(ctx->txn.undo[i].orig); j++)
			ctx->txn.undo[i].orig[j] = 0;
	}
}

int dh_txn_begin(struct dh_ctx *ctx)
{
	if (!ctx || !ctx->initialized || !ctx->be)
		return DH_E_INVAL;
	if (ctx->txn.active)
		return DH_E_BUSY;

	dh_txn_reset(ctx);
	ctx->txn.active = 1;
	return DH_OK;
}

int dh_txn_record(struct dh_ctx *ctx, void *addr, size_t len,
		  enum dh_route route)
{
	struct dh_undo_ent *ent;
	size_t cap;
	int ret;

	if (!ctx || !ctx->initialized || !ctx->be)
		return DH_E_INVAL;
	if (!ctx->txn.active)
		return DH_E_INVAL;
	if (!addr || !len)
		return DH_E_INVAL;

	cap = sizeof(ctx->txn.undo[0].orig);
	if (len > cap)
		return DH_E_INVAL;
	if (!dh_route_valid(route))
		return DH_E_INVAL;
	/* Never grow a transaction that already failed a read-back. */
	if (ctx->txn.failed)
		return DH_E_VERIFY;
	if (ctx->txn.n >= (u32)DH_UNDO_SLOTS)
		return DH_E_NOSPC;

	ent = &ctx->txn.undo[ctx->txn.n];
	ret = dh_read(ctx, addr, ent->orig, len);
	if (ret != DH_OK)
		return ret;

	ent->addr = addr;
	ent->len = len;
	ent->route = route;
	ent->used = 1;
	ctx->txn.n++;
	return DH_OK;
}

/*
 * Reverse replay.  The quiesce window is opened by the caller, so this helper
 * can be shared by dh_txn_rollback() and dh_ctx_fini() without nesting two
 * pairs of quiesce_begin/end.
 *
 * dh.h records only the original bytes and the route.  It has no field for
 * "which primitive made the change", so replay always goes through ro_write
 * with the recorded route; for the four RO paths that reaches the same
 * physical target.  This is reported to the lead as a contract gap.
 */
int dh_txn_rollback_locked(struct dh_ctx *ctx)
{
	struct dh_backend *be;
	unsigned char back[sizeof(((struct dh_ctx *)0)->txn.undo[0].orig)] = { 0 };
	u32 i;
	int first_err = DH_OK;

	if (!ctx || !ctx->be)
		return DH_E_INVAL;
	be = ctx->be;

	for (i = ctx->txn.n; i > 0; i--) {
		struct dh_undo_ent *ent = &ctx->txn.undo[i - 1];
		int ret;

		if (!ent->used)
			continue;

		ret = be->ro_write(ent->addr, ent->orig, ent->len, ent->route);
		if (ret == DH_OK) {
			ret = dh_read(ctx, ent->addr, back, ent->len);
			if (ret == DH_OK &&
			    !dh_undo_bytes_equal(back, ent->orig, ent->len))
				ret = DH_E_VERIFY;
		}
		/* Keep replaying: a partial rollback is worse than a full
		 * best-effort one.  Report the first error only. */
		if (ret != DH_OK && first_err == DH_OK)
			first_err = ret;
	}

	/* Clear the log even after a partial failure; replaying the same undo
	 * entries twice would be worse than returning an error. */
	ctx->txn.n = 0;
	ctx->txn.active = 0;
	ctx->txn.failed = 0;
	for (i = 0; i < (u32)DH_UNDO_SLOTS; i++)
		ctx->txn.undo[i].used = 0;
	return first_err;
}

int dh_txn_rollback(struct dh_ctx *ctx)
{
	struct dh_backend *be;
	int ret;

	if (!ctx || !ctx->initialized || !ctx->be)
		return DH_E_INVAL;
	if (!ctx->txn.active)
		return DH_OK;
	be = ctx->be;
	if (!be->quiesce_begin || !be->quiesce_end)
		return DH_E_QUIESCE;

	be->quiesce_begin();
	ret = dh_txn_rollback_locked(ctx);
	be->quiesce_end();
	return ret;
}

int dh_txn_commit(struct dh_ctx *ctx)
{
	if (!ctx || !ctx->initialized || !ctx->be)
		return DH_E_INVAL;
	if (!ctx->txn.active)
		return DH_E_INVAL;

	/*
	 * dh_write() already verifies every write by reading it back and sets
	 * txn->failed on mismatch.  Because struct dh_undo_ent stores only the
	 * original bytes, commit cannot independently re-verify the new
	 * contents; it relies on that flag.  A failed transaction is never
	 * committed: it is rolled back instead.
	 */
	if (ctx->txn.failed) {
		int rb = dh_txn_rollback(ctx);

		return rb == DH_OK ? DH_E_VERIFY : rb;
	}

	/* Clearing the undo log is the commit: no writes are replayed. */
	dh_txn_reset(ctx);
	return DH_OK;
}
