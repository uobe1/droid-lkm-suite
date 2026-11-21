/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Droid-Hook <obj,id> side table.
 *
 * Same family as upstream klp_shadow, KernelPatch kstorage and KPM task_ext
 * (docs/evidence/rw-memory-mutation-research.md section 2.1-C and section
 * 4.1): the parent object's layout is never changed, the extra state lives in
 * a separately allocated item keyed by (object address, id), and freeing is
 * delayed so an in-flight reader cannot observe freed memory.
 *
 * The kernel build would use a RCU hashtable plus call_rcu/kfree_rcu.  The
 * host build has no RCU, so a bounded linear table with a refcount stands in
 * for the same lifecycle: free() detaches the item from the table immediately
 * (a new get-or-alloc therefore returns a fresh object) but only reclaims the
 * storage once the last reference is dropped.  No parent object is touched.
 */

#include "dh.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define DH_SHADOW_MAX		32
#define DH_SHADOW_ALIVE		0x53484441U	/* "SHDA" */

struct dh_shadow_slot {
	struct dh_shadow_item *item;
	void *obj;
	u32 id;
	size_t size;
	u32 refs;
	u8 detached;
	u8 used;
};

static struct dh_shadow_slot dh_shadow_tab[DH_SHADOW_MAX];
static unsigned char dh_shadow_mem[DH_SHADOW_MAX][256];
static u8 dh_shadow_mem_used[DH_SHADOW_MAX];
static u32 dh_shadow_gen;

static int dh_shadow_key_eq(const struct dh_shadow_slot *s, void *obj, u32 id)
{
	/* A detached slot must not shadow a fresh get-or-alloc for the same
	 * key: free() semantically removes the item from the table even while
	 * delayed reclaim keeps the old storage alive for in-flight readers. */
	return s->used && !s->detached && s->obj == obj && s->id == id;
}

static struct dh_shadow_slot *dh_shadow_find(void *obj, u32 id)
{
	size_t i;

	for (i = 0; i < DH_SHADOW_MAX; i++)
		if (dh_shadow_key_eq(&dh_shadow_tab[i], obj, id))
			return &dh_shadow_tab[i];
	return NULL;
}

static struct dh_shadow_item *dh_shadow_alloc(struct dh_ctx *ctx, size_t size)
{
	struct dh_shadow_item *item;
	size_t i, total = sizeof(*item) + size;

	if (ctx && ctx->be && ctx->be->alloc) {
		item = (struct dh_shadow_item *)ctx->be->alloc(total);
		if (item) {
			memset(item, 0, total);
			return item;
		}
	}
	/* Static fallback: keeps get-or-alloc allocation-free on the host and
	 * in a panic path.  Items larger than the fixed cell fall back to
	 * DH_E_NOSPC at the caller. */
	if (total > sizeof(dh_shadow_mem[0]))
		return NULL;
	for (i = 0; i < DH_SHADOW_MAX; i++) {
		if (!dh_shadow_mem_used[i]) {
			dh_shadow_mem_used[i] = 1;
			item = (struct dh_shadow_item *)dh_shadow_mem[i];
			memset(item, 0, total);
			return item;
		}
	}
	return NULL;
}

static int dh_in_pool_shadow(const void *p)
{
	const unsigned char *a = (const unsigned char *)dh_shadow_mem;
	const unsigned char *x = (const unsigned char *)p;

	return x >= a && x < a + sizeof(dh_shadow_mem);
}

static void dh_shadow_free_item(struct dh_ctx *ctx, struct dh_shadow_item *item)
{
	if (!item)
		return;
	if (dh_in_pool_shadow(item)) {
		size_t i = (size_t)(((const unsigned char *)item -
				     (const unsigned char *)dh_shadow_mem) /
				    sizeof(dh_shadow_mem[0]));
		if (i < DH_SHADOW_MAX)
			dh_shadow_mem_used[i] = 0;
	} else if (ctx && ctx->be && ctx->be->free) {
		ctx->be->free(item);
	}
}

static void dh_shadow_reclaim(struct dh_ctx *ctx, struct dh_shadow_slot *s)
{
	dh_shadow_free_item(ctx, s->item);
	memset(s, 0, sizeof(*s));
}

static struct dh_shadow_slot *dh_shadow_empty(void)
{
	size_t i;

	for (i = 0; i < DH_SHADOW_MAX; i++)
		if (!dh_shadow_tab[i].used)
			return &dh_shadow_tab[i];
	return NULL;
}

/**
 * dh_shadow_get_or_alloc() - look up or create side state for (obj, id)
 * @ctx: library context, supplies alloc/free when present
 * @obj: parent object address, used as the key, never dereferenced
 * @id: caller-chosen id, allows several independent items per object
 * @size: bytes of trailing data in the flexible array
 *
 * Idempotent: a repeated call with the same (obj, id) returns the very same
 * pointer and keeps the original size.  A size mismatch on an existing key is
 * rejected rather than silently reallocating, which would invalidate readers
 * that still hold the pointer.  The parent object's layout is untouched.
 *
 * Return: the item on success, or %NULL on invalid arguments, out of table
 * space, or a size mismatch.  NULL is the documented failure indicator for
 * pointer-returning functions in dh.h.
 */
struct dh_shadow_item *dh_shadow_get_or_alloc(struct dh_ctx *ctx, void *obj,
					       u32 id, size_t size)
{
	struct dh_shadow_slot *s;
	struct dh_shadow_item *item;

	if (!obj || !size)
		return NULL;
	s = dh_shadow_find(obj, id);
	if (s) {
		if (s->detached || s->size != size)
			return NULL;
		s->refs++;
		return s->item;
	}
	item = dh_shadow_alloc(ctx, size);
	if (!item)
		return NULL;
	s = dh_shadow_empty();
	if (!s) {
		/* Put the freshly allocated storage back before failing. */
		dh_shadow_free_item(ctx, item);
		return NULL;
	}
	item->obj = obj;
	item->id = id;
	item->size = size;
	s->item = item;
	s->obj = obj;
	s->id = id;
	s->size = size;
	s->refs = 1;
	s->detached = 0;
	s->used = 1;
	dh_shadow_gen++;
	return item;
}

/**
 * dh_shadow_free() - detach one (obj, id) item and start delayed reclaim
 * @ctx: library context
 * @obj: parent object address used at allocation
 * @id: id used at allocation
 *
 * Detaches the item from the table immediately, so a subsequent
 * get_or_alloc() for the same key returns a new object.  The storage itself
 * is only reclaimed once the last reference goes away; on the host the
 * refcount is dropped by dh_shadow_put().  Nothing is written into @obj.
 *
 * Return: %DH_OK, %DH_E_INVAL, or %DH_E_NOENT.
 */
int dh_shadow_free(struct dh_ctx *ctx, void *obj, u32 id)
{
	struct dh_shadow_slot *s;

	if (!obj)
		return DH_E_INVAL;
	s = dh_shadow_find(obj, id);
	if (!s || s->detached)
		return DH_E_NOENT;
	s->detached = 1;
	dh_shadow_gen++;
	/* A detached slot with no remaining readers is reclaimable at once. */
	if (s->refs <= 1) {
		s->refs = 0;
		dh_shadow_reclaim(ctx, s);
	}
	return DH_OK;
}

/**
 * dh_shadow_put() - drop one reference obtained from get_or_alloc
 * @ctx: library context
 * @item: item previously returned by dh_shadow_get_or_alloc()
 *
 * Host-side stand-in for the RCU grace period: the item stays alive until
 * the last reference is dropped, even after dh_shadow_free() detached it.
 *
 * Return: %DH_OK or %DH_E_INVAL.
 */
int dh_shadow_put(struct dh_ctx *ctx, struct dh_shadow_item *item)
{
	size_t i;
	struct dh_shadow_slot *s = NULL;

	if (!item)
		return DH_E_INVAL;
	for (i = 0; i < DH_SHADOW_MAX; i++) {
		if (dh_shadow_tab[i].used &&
		    dh_shadow_tab[i].item == item) {
			s = &dh_shadow_tab[i];
			break;
		}
	}
	if (!s || s->refs == 0)
		return DH_E_INVAL;
	s->refs--;
	if (s->refs == 0 && s->detached)
		dh_shadow_reclaim(ctx, s);
	return DH_OK;
}

/* ---- internal test surface (not part of dh.h) ---- */

void dh_shadow_test_reset(void)
{
	memset(dh_shadow_tab, 0, sizeof(dh_shadow_tab));
	memset(dh_shadow_mem, 0, sizeof(dh_shadow_mem));
	memset(dh_shadow_mem_used, 0, sizeof(dh_shadow_mem_used));
	dh_shadow_gen = 0;
}

u32 dh_shadow_test_generation(void)
{
	return dh_shadow_gen;
}

int dh_shadow_test_live(void)
{
	size_t i;
	int n = 0;

	for (i = 0; i < DH_SHADOW_MAX; i++)
		if (dh_shadow_tab[i].used)
			n++;
	return n;
}
