/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DROID_HOOK_DH_H
#define _DROID_HOOK_DH_H

/**
 * DOC: Droid-Hook library contract
 *
 * Droid-Hook is a library that can be embedded into an LKM or any other
 * kernel module.  It owns no global state: every entry point takes a
 * struct dh_ctx whose ->be points at the runtime's backend.  The runtime
 * (by default dnsk_core.ko's rw/patch_pointer single-entry ABI 5) supplies
 * the backend; swapping the runtime means supplying a different backend,
 * with zero changes to the library.
 *
 * This header only declares the contract.  It must compile in both kernel
 * and host (userspace selftest) contexts; kernel-only types are guarded by
 * __KERNEL__ and mirrored by plain fixed-width types on the host.
 */

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/**
 * define DH_ABI - Droid-Hook library ABI revision
 *
 * Bumped whenever struct dh_backend, struct dh_ctx or a public signature
 * changes.  dh_ctx_init() rejects a backend whose abi does not match.
 */
#define DH_ABI 1
/**
 * define DH_KEY_LEN - maximum hook key including the NUL
 *
 * Shared with the legacy dnsk_core hook registry so keys are interchangeable.
 */
#define DH_KEY_LEN 48
/**
 * define DH_UNDO_SLOTS - number of undo entries in one transaction
 *
 * Fixed and statically allocated so a rollback in a panic path never
 * allocates.  dh_txn_record() returns -ENOSPC when the log is full.
 */
#define DH_UNDO_SLOTS 8
/**
 * define DH_MAX_BATCH - maximum instructions in one text-patch batch
 *
 * One stop_machine window amortises the whole batch; larger sets must be
 * split into multiple transactions.
 */
#define DH_MAX_BATCH 64

/**
 * enum dh_route - how a read-only target is written
 *
 * The route is chosen per transaction step.  ALIAS is preferred because it
 * leaves the original PTE untouched; PTE is the KernelPatch hotpatch path;
 * FIXMAP mirrors arm64 patch_map(); DMA is the EL2/Stage-2 fallback from the
 * prior-art survey.  Each has the same post-write contract: dsb(ish),
 * icache flush when the target is executable, TLB flush, read-back verify.
 */
enum dh_route {
	DH_ROUTE_ALIAS = 1,
	DH_ROUTE_PTE,
	DH_ROUTE_FIXMAP,
	DH_ROUTE_DMA,
};

/**
 * enum dh_ret - unified return codes
 *
 * Negative values are errnos so callers can propagate them unchanged.
 * DH_E_VERIFY means the write was issued but the read-back did not match;
 * the transaction must be rolled back, never treated as success.
 */
enum dh_ret {
	DH_OK = 0,
	DH_E_INVAL = -22,
	DH_E_NOSPC = -28,
	DH_E_BUSY = -16,
	DH_E_NOENT = -2,
	DH_E_OPNOTSUPP = -95,
	DH_E_VERIFY = -1000,
	DH_E_QUIESCE = -1001,
};

/**
 * struct dh_rw_req - one backend memory operation
 *
 * Mirrors the shape of struct dnsk_rw_req but is owned by Droid-Hook so the
 * library never includes a core header.  The backend adapts this to whatever
 * primitive the runtime provides.  @len is capped at one page by the
 * runtime; the library splits larger transfers itself.
 */
struct dh_rw_req {
	u16 opcode;
	u16 flags;
	u32 lock_type;
	void *addr;
	const void *src;
	void *dst;
	void *lock;
	size_t len;
	u64 value;
	u64 old;
	int ret;
};

/**
 * struct dh_backend - runtime-provided primitives
 *
 * The runtime fills this once and hands it to dh_ctx_init().  Every function
 * pointer is mandatory except @alloc/@free, which may be NULL when the
 * library only uses caller-supplied buffers.  @text_write must sleep-safe
 * (stop_machine + fixmap); @rw must be callable from lock-held context for
 * the data path.
 */
struct dh_backend {
	int (*rw)(struct dh_rw_req *req);
	int (*patch_pointer)(void *target, void *value);
	int (*text_write)(void *addr, const void *src, size_t len,
			  enum dh_route route);
	int (*ro_write)(void *addr, const void *src, size_t len,
			enum dh_route route);
	void *(*probe)(const char *name);
	void *(*alloc)(size_t size);
	void (*free)(const void *p);
	void (*icache_flush)(void *addr, size_t len);
	void (*tlb_flush)(void *addr, size_t len);
	void (*quiesce_begin)(void);
	void (*quiesce_end)(void);
	const char *name;
	u16 abi;
};

/**
 * struct dh_undo_ent - one recorded mutation for rollback
 *
 * An undo entry stores the original bytes, not a description of the write,
 * so rollback is a plain reverse replay.  @route is recorded because the
 * undo must use the same route that made the change.
 */
struct dh_undo_ent {
	void *addr;
	u8 orig[256];
	size_t len;
	enum dh_route route;
	u8 used;
};

/**
 * struct dh_txn - one atomic patch transaction
 *
 * dh_txn_begin() snapshots nothing; each dh_txn_record() copies @len bytes
 * from @addr before the caller writes.  dh_txn_commit() clears the undo log
 * after read-back verification; dh_txn_rollback() replays it in reverse
 * inside one quiesce window.  The log is intentionally fixed-size and
 * statically allocated so a panic path never needs to allocate.
 */
struct dh_txn {
	struct dh_ctx *ctx;
	struct dh_undo_ent undo[DH_UNDO_SLOTS];
	u32 n;
	u8 active;
	u8 failed;
};

/**
 * struct dh_ctx - library instance
 *
 * One per embedding module.  @be is borrowed, not copied: the runtime must
 * outlive every ctx that references it.  @key namespace is shared with the
 * legacy dnsk_core hook registry so the two cannot install a hook at the
 * same address; @addr_arbitrate is called by dh_hook_install() to enforce it.
 */
struct dh_ctx {
	struct dh_backend *be;
	struct dh_txn txn;
	u8 initialized;
	char owner[DH_KEY_LEN];
	int (*addr_arbitrate)(void *addr, const char *key);
};

/**
 * struct dh_hook - one installed function-level hook
 *
 * @trampoline points at the relo buffer (original instructions relocated
 * plus a jump back to original+4-insn).  @chain is the before/after callback
 * list, before in registration order, after in reverse.
 */
struct dh_hook {
	char key[DH_KEY_LEN];
	void *target;
	void *trampoline;
	void *chain;
	u32 nr_chain;
	u8 installed;
};

/**
 * struct dh_shadow_item - one <obj,id> side-table entry
 *
 * RCU-protected in the kernel build.  The object address is the key, so the
 * parent struct layout never changes; this is the klp_shadow/KPM task_ext
 * pattern from the design evidence.
 */
struct dh_shadow_item {
	void *obj;
	u32 id;
	size_t size;
	u8 data[];
};

/* ---- lifecycle ---- */

/**
 * dh_ctx_init() - bind a library instance to a runtime backend
 * @ctx: caller-owned context, zeroed before the call
 * @be: runtime backend, must outlive @ctx
 * @owner: stable name for diagnostics, copied into @ctx
 *
 * Return: %DH_OK, or %DH_E_INVAL when @be lacks a mandatory primitive.
 */
int dh_ctx_init(struct dh_ctx *ctx, struct dh_backend *be, const char *owner);

/**
 * dh_ctx_fini() - quiesce, roll back any open transaction, release ctx
 * @ctx: context previously passed to dh_ctx_init()
 *
 * Return: %DH_OK on success, %DH_E_QUIESCE when the runtime could not
 * quiesce in-flight callers; the caller must retry, not force-unload.
 */
int dh_ctx_fini(struct dh_ctx *ctx);

/* ---- primitive wrappers ---- */

int dh_read(struct dh_ctx *ctx, void *addr, void *dst, size_t len);
int dh_write(struct dh_ctx *ctx, void *addr, const void *src, size_t len);
int dh_xchg64(struct dh_ctx *ctx, void *addr, u64 value, u64 *old);
int dh_cas64(struct dh_ctx *ctx, void *addr, u64 expected, u64 value);
void *dh_probe(struct dh_ctx *ctx, const char *name);

/* ---- transaction ---- */

int dh_txn_begin(struct dh_ctx *ctx);
int dh_txn_record(struct dh_ctx *ctx, void *addr, size_t len,
		  enum dh_route route);
int dh_txn_commit(struct dh_ctx *ctx);
int dh_txn_rollback(struct dh_ctx *ctx);

/* ---- instruction-level patch ---- */

int dh_insn_gen_absolute_jump(u32 *out, void *target);
int dh_insn_relocate(const u32 *src, u32 *dst, void *src_pc, void *dst_pc,
		     size_t nr);
int dh_scan_refs(struct dh_ctx *ctx, void *old_base, size_t old_len,
		 void *new_base, void **sites, size_t max_sites);
int dh_patch_text_batch(struct dh_ctx *ctx, void **addrs, u32 **insns,
			size_t *lens, size_t nr);

/* ---- hook ---- */

int dh_hook_install(struct dh_ctx *ctx, const char *key, void *target,
		    void *replacement, struct dh_hook *out);
int dh_hook_uninstall(struct dh_ctx *ctx, struct dh_hook *hook);
int dh_hook_wrap(struct dh_ctx *ctx, struct dh_hook *hook,
		 int (*before)(void *fargs), int (*after)(void *fargs));

/* ---- shadow / side table ---- */

struct dh_shadow_item *dh_shadow_get_or_alloc(struct dh_ctx *ctx, void *obj,
					       u32 id, size_t size);
int dh_shadow_free(struct dh_ctx *ctx, void *obj, u32 id);

/* ---- protection probe / preflight ---- */

int dh_preflight(struct dh_ctx *ctx);
int dh_probe_protection(struct dh_ctx *ctx, void *addr, enum dh_route *route);

#endif /* _DROID_HOOK_DH_H */
