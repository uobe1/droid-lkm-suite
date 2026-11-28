/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_CORE_H
#define _DNSK_CORE_H

#include <linux/types.h>

/**
 * define DNSK_CORE_MAGIC - identifies a valid struct dnsk_core_api
 *
 * Feature modules compare this value against the @magic field before using
 * any function pointer from the API, so that an unrelated struct returned by a
 * stale or replaced core is rejected instead of dereferenced.
 */
#define DNSK_CORE_MAGIC 0x444E534BU

/**
 * define DNSK_CORE_ABI - ABI revision of struct dnsk_core_api
 *
 * Passed by the feature module to dnsk_core_get().  The core refuses the
 * request unless the caller's value matches exactly; the value changes
 * whenever the layout or semantics of the API change.
 */
#define DNSK_CORE_ABI 2

struct module;

/**
 * define DNSK_FEATURE_NAME_LEN - maximum feature name including the NUL
 *
 * dnsk_feature_add() rejects a longer name with -ENAMETOOLONG.  Names are
 * stored by value, the caller's string need not stay alive.
 */
#define DNSK_FEATURE_NAME_LEN 32

/**
 * define DNSK_HOOK_KEY_LEN - maximum hook key including the NUL
 *
 * A key at or above this length is rejected with -EINVAL by
 * hook_install().  The key is copied into the hook registry and is what
 * hook_uninstall() later uses to identify the hook.
 */
#define DNSK_HOOK_KEY_LEN 48

/**
 * enum dnsk_status - implementation level reported for a feature
 *
 * The level a feature module claims in &struct dnsk_feature_desc.  It is
 * informational: core stores it and prints it through the diag interface, it
 * does not change how the feature behaves.  The single letters in the field
 * descriptions match the levels defined in the project status document.
 */
enum dnsk_status {
	/**
	 * @DNSK_STATUS_UNKNOWN: value zero, no level claimed.
	 *
	 * This is the value a zeroed structure holds; it is not a level.  It
	 * also is what dnsk_status_name() returns "unknown" for.
	 */
	DNSK_STATUS_UNKNOWN = 0,
	/** @DNSK_STATUS_NOT_SHIPPED: not built or not yet usable. */
	DNSK_STATUS_NOT_SHIPPED,
	/** @DNSK_STATUS_FEASIBILITY: under a feasibility gate, no contract yet. */
	DNSK_STATUS_FEASIBILITY,
	/** @DNSK_STATUS_DEVELOPMENT: implemented, acceptance not complete. */
	DNSK_STATUS_DEVELOPMENT,
	/**
	 * @DNSK_STATUS_NATIVE: N, the host already has it, or it matches the
	 * AOSP Native oracle over the full acceptance matrix.
	 */
	DNSK_STATUS_NATIVE,
	/**
	 * @DNSK_STATUS_REAL: R, ported from source of the same KMI, the diff
	 * is auditable and the workload passes.
	 */
	DNSK_STATUS_REAL,
	/**
	 * @DNSK_STATUS_DIFFERENTIAL: D, parity with the AOSP Native oracle
	 * inside the defined contract.
	 */
	DNSK_STATUS_DIFFERENTIAL,
	/**
	 * @DNSK_STATUS_VIRTUAL: V, real state, enforcement and files, but not
	 * a native kernel cgroup_subsys.
	 */
	DNSK_STATUS_VIRTUAL,
	/** @DNSK_STATUS_NA: the feature does not exist upstream for this KMI. */
	DNSK_STATUS_NA,
	/** @DNSK_STATUS_UNSUPPORTED: this KMI does not support the feature. */
	DNSK_STATUS_UNSUPPORTED,
};

/**
 * struct dnsk_inline_hook_req - one inline hook installation request
 *
 * Passed to hook_install().  Core resolves @symbol and writes the callable
 * original-function block through @origin_out before it publishes the entry
 * jump to @replacement.  When @origin_out is non-NULL but no executable
 * origin can be built, hook_install() returns -EOPNOTSUPP before writing
 * any text.  The strings must stay valid only for the duration of the call,
 * except @key which is copied.  Core does not pin @owner, so the registering
 * module must call hook_uninstall() for every key it installed in
 * module_exit(); the reference the module holds on core through
 * dnsk_core_get() keeps core itself alive until then.
 */
struct dnsk_inline_hook_req {
	/** @key: unique name of the hook, shorter than DNSK_HOOK_KEY_LEN. */
	const char *key;
	/** @symbol: kernel symbol name whose entry is hooked. */
	const char *symbol;
	/**
	 * @owner: registering module, for diagnostics.
	 *
	 * Core does not pin it.  The owner must uninstall every hook in
	 * module_exit().
	 */
	struct module *owner;
	/**
	 * @replacement: same-prototype continuation function.
	 *
	 * Its signature must exactly match @symbol.  To continue the original
	 * logic, it calls the block returned through @origin_out when that
	 * pointer is non-NULL.
	 */
	void *replacement;
	/**
	 * @origin_out: optional out-parameter for the original-function block.
	 *
	 * When non-NULL, receives the callable relocated-origin block before
	 * the entry jump is published; if no executable origin can be built,
	 * hook_install() fails with -EOPNOTSUPP before writing any text.  The
	 * block is relocated machine code without a kCFI type hash, so a normal
	 * indirect call to it fails the CFI check; on CFI kernels the
	 * replacement must be built with __nocfi (or otherwise suppress the
	 * check on that call).  The block stays valid until hook_uninstall()
	 * for @key returns.  When NULL, the replacement fully replaces @symbol
	 * and no origin is provided.
	 */
	void **origin_out;
};

/**
 * struct dnsk_feature_desc - one feature registration request
 *
 * Passed to dnsk_feature_add(), which copies every string; the caller's
 * buffers do not need to outlive the call.  Registrations are informational,
 * they do not enable or install anything.
 */
struct dnsk_feature_desc {
	/** @name: unique feature name, shorter than DNSK_FEATURE_NAME_LEN. */
	const char *name;
	/** @version: optional version string, may be %NULL. */
	const char *version;
	/** @status: implementation level claimed by the feature. */
	enum dnsk_status status;
	/** @caps: optional capabilities string, may be %NULL. */
	const char *caps;
	/**
	 * @owner: registering module, for diagnostics.
	 *
	 * Core does not pin it.  The owner must remove its registration in
	 * module_exit().
	 */
	struct module *owner;
};

/**
 * enum dnsk_rw_opcode - operation selected by struct dnsk_rw_req
 *
 * READ and WRITE copy bytes; XCHG and CAS are atomic read-modify-write;
 * PROBE resolves src as a symbol name and stores the address in dst.
 */
enum dnsk_rw_opcode {
	/** @DNSK_RW_READ: copy from req->addr to req->dst. */
	DNSK_RW_READ = 1,
	/** @DNSK_RW_WRITE: copy from req->src to req->addr. */
	DNSK_RW_WRITE,
	/** @DNSK_RW_XCHG: exchange req->value with the u64 at req->addr. */
	DNSK_RW_XCHG,
	/** @DNSK_RW_CAS: compare against req->old before writing req->value. */
	DNSK_RW_CAS,
	/** @DNSK_RW_PROBE: resolve the symbol named by req->src into req->dst. */
	DNSK_RW_PROBE,
};

/**
 * enum dnsk_rw_lock_type - lock core takes when DNSK_RW_F_LOCKED is set
 *
 * The caller must not already hold that lock; lock-held contexts use the
 * lockless data path instead.
 */
enum dnsk_rw_lock_type {
	/** @DNSK_RW_LOCK_NONE: no lock is taken. */
	DNSK_RW_LOCK_NONE = 0,
	/** @DNSK_RW_LOCK_MUTEX: req->lock points at a struct mutex. */
	DNSK_RW_LOCK_MUTEX,
	/** @DNSK_RW_LOCK_SPIN_IRQSAVE: req->lock points at a spinlock_t. */
	DNSK_RW_LOCK_SPIN_IRQSAVE,
	/** @DNSK_RW_LOCK_RWSEM_READ: req->lock points at a struct rw_semaphore. */
	DNSK_RW_LOCK_RWSEM_READ,
};

/**
 * define DNSK_RW_F_LOCKED - perform the operation while holding req->lock
 *
 * The caller must not already hold that lock; for lock-held contexts use the
 * lockless data path.
 */
#define DNSK_RW_F_LOCKED (1u << 0)
/**
 * define DNSK_RW_F_TEXT - the target is executable text
 *
 * Flush the instruction cache after writing.
 */
#define DNSK_RW_F_TEXT (1u << 1)
/**
 * define DNSK_RW_F_RO - the target may be read-only
 *
 * Force the patch path, which sleeps and must not be used while holding a
 * spinlock.  Without this flag XCHG/CAS/WRITE use hardware atomics or a
 * direct store.
 */
#define DNSK_RW_F_RO (1u << 2)

/**
 * struct dnsk_rw_req - one kernel read, write, exchange or probe
 *
 * One request describes one operation.  The common case has no extra cost:
 * READ and WRITE go straight to the copy or patch path, and no global lock is
 * taken unless DNSK_RW_F_LOCKED is set.  @ret receives the result so callers
 * can batch several requests through one dispatch.
 *
 * Addresses are discovered with DNSK_RW_PROBE, which resolves @src as a symbol
 * name and stores the address in @dst.  Code must not hard-code per-device
 * addresses.
 */
struct dnsk_rw_req {
	/** @opcode: one of &enum dnsk_rw_opcode. */
	u16 opcode;
	/** @flags: DNSK_RW_F_LOCKED or 0. */
	u16 flags;
	/** @lock_type: one of &enum dnsk_rw_lock_type. */
	u32 lock_type;
	/** @addr: kernel address for READ/WRITE/XCHG/CAS. */
	void *addr;
	/** @src: source buffer, or the symbol name for PROBE. */
	const void *src;
	/** @dst: destination buffer, or `void **` for PROBE. */
	void *dst;
	/** @lock: mutex or spinlock address when DNSK_RW_F_LOCKED is set. */
	void *lock;
	/** @len: byte count, at most 256 and within one page. */
	size_t len;
	/** @value: new value for XCHG/CAS. */
	u64 value;
	/** @old: previous value read by XCHG, or expected value for CAS. */
	u64 old;
	/** @ret: result of the operation. */
	int ret;
};

/**
 * struct dnsk_core_api - function table returned by dnsk_core_get()
 *
 * The core fills this once at init and hands out a pointer to it.  A caller
 * must validate @magic and @abi before dereferencing any function pointer;
 * dnsk_core_get() already does both, so they are only needed if the structure
 * is reached by other means.
 */
struct dnsk_core_api {
	/** @magic: must equal DNSK_CORE_MAGIC. */
	u32 magic;
	/** @abi: must equal the DNSK_CORE_ABI the caller was built against. */
	u16 abi;
	/** @size: size of this structure in bytes. */
	u16 size;
	/**
	 * @resolve: look up a kernel symbol by name.
	 *
	 * @name: symbol name, e.g. ``"sys_call_table"``.
	 *
	 * Resolves through kallsyms at runtime, so core imports no non-exported
	 * symbol itself.  On kernels before 6.1 a direct lookup that fails is
	 * retried against the KCFI ``<name>.cfi_jt`` jump table entry.
	 *
	 * Return: the symbol address, or %NULL when it cannot be resolved.
	 */
	void *(*resolve)(const char *name);
	/**
	 * @hook_install: install an inline hook at a kernel symbol entry.
	 *
	 * @req: the hook to install.
	 *
	 * Resolves @req->symbol, writes @req->origin_out before publishing the
	 * entry jump to @req->replacement, and then installs that jump through
	 * droid-hook.  The replacement must have the target's exact prototype
	 * and call the origin block to continue the original logic.
	 *
	 * Return: %0 on success, negative errno on failure, including -EINVAL
	 * for a malformed request, -ENOENT when @symbol cannot be resolved,
	 * -EBUSY for a duplicate @key or a second hook at the same target,
	 * -ENOSPC when the registry is full, and -EOPNOTSUPP when the patching
	 * backend or the executable-origin allocator is unavailable.
	 */
	int (*hook_install)(const struct dnsk_inline_hook_req *req);
	/**
	 * @hook_uninstall: remove an inline hook by key and restore its entry.
	 *
	 * @key: the key the hook was installed with.
	 *
	 * Restores the original entry bytes, then waits for an RCU grace period
	 * on success, so a caller may free handler data on return.  The origin
	 * block is retired, not freed (see unload-safety).
	 *
	 * Return: %0 on success, -EINVAL for a %NULL @key, -ENOENT when no
	 * hook has that key, or the error from restoring the original.
	 */
	int (*hook_uninstall)(const char *key);
	/**
	 * @feature_add: record a feature registration.
	 *
	 * @desc: the registration to record.
	 *
	 * Return: %0 on success, -EINVAL for a missing @name, @owner or empty
	 * name, -ENAMETOOLONG for an over-long name, -EEXIST when the name is
	 * taken, or -ENOSPC when the registry is full.
	 */
	int (*feature_add)(const struct dnsk_feature_desc *desc);
	/**
	 * @feature_del: drop a feature registration by name.
	 *
	 * @name: the name the feature was registered with.
	 *
	 * Return: %0 on success, -EINVAL for a %NULL @name, or -ENOENT when
	 * no feature has that name.
	 */
	int (*feature_del)(const char *name);
	/**
	 * @log: formatted log line through the core logger.
	 *
	 * @level: log level; 0 logs an error, 1 a warning, any other value
	 *         logs info.
	 * @fmt: printf-style format, rendered into a 256-byte stack buffer and
	 *       therefore truncated past that; a %NULL @fmt logs nothing.
	 * @...: arguments for @fmt.
	 *
	 * Context: Any context.  The line is emitted through printk, so the
	 * usual printk rules apply.
	 */
	void (*log)(int level, const char *fmt, ...);
	/**
	 * @patch_pointer: write an aligned pointer into kernel memory.
	 *
	 * @target: address of the pointer slot to write.
	 * @value: pointer value to store.
	 *
	 * Uses the patching backend (stop_machine + fixmap), not a plain store,
	 * so the target may live in read-only-after-init memory.
	 *
	 * Return: %0 on success, -EINVAL for a %NULL @target, or -EOPNOTSUPP
	 * when the patching backend is not available.
	 */
	int (*patch_pointer)(void *target, void *value);
	/**
	 * @rw: dispatch one struct dnsk_rw_req.
	 *
	 * @req: the request to execute; req->ret receives the result.
	 *
	 * READ/WRITE/XCHG/CAS use a direct copy or hardware atomics and do not
	 * enter stop_machine, so they are safe in lock-held contexts.  F_TEXT
	 * and F_RO route through the sleeping patch backend.
	 *
	 * Return: %0 on success, negative errno on failure.
	 */
	int (*rw)(struct dnsk_rw_req *req);
	/**
	 * @owner: the core module, with one reference held for the caller.
	 *
	 * dnsk_core_get() returns with one reference taken on this module.  The
	 * caller must keep it for as long as the API may be used and release it
	 * with module_put(owner); the API must not be touched after that final
	 * put.
	 */
	struct module *owner;
};

/**
 * dnsk_core_get() - obtain the core API function table
 * @abi: ABI revision the caller was built against, normally DNSK_CORE_ABI.
 * @size: size of the caller's &struct dnsk_core_api, normally sizeof().
 *
 * Validates the caller against the running core and, on success, takes a
 * reference on the core module so the table stays valid while it is in use.
 * Release that reference with module_put() on the returned @owner once the
 * API is no longer needed.
 *
 * Context: Process context.  May be called from module_init().
 *
 * Return: the API table on success, or an ERR_PTR() value: -ENODEV when the
 * core is not ready, -EPROTO when @abi or @size does not match.
 */
struct dnsk_core_api *dnsk_core_get(u16 abi, u16 size);

#endif /* _DNSK_CORE_H */
