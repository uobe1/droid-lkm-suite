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
#define DNSK_CORE_ABI 5

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
 * A key at or above this length is rejected with -EINVAL by dnsk_hook_add().
 * The key is copied into the hook registry and is what dnsk_hook_del() later
 * uses to identify the hook.
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
 * enum dnsk_hook_type - kind of hook requested from the core
 *
 * Selects which fields of &struct dnsk_hook_req are meaningful and how core
 * installs and removes the hook.  Core owns every hook it installs, so a
 * feature module cannot leave a permanent hook behind on unload.
 */
enum dnsk_hook_type {
	/**
	 * @DNSK_HOOK_KPROBE: kprobe on @symbol.
	 *
	 * @replacement is the pre-handler, typed
	 * ``int (*)(struct kprobe *, struct pt_regs *)``; returning non-zero
	 * skips the probed instruction.  @original_out receives the probed
	 * address, not a function pointer.
	 */
	DNSK_HOOK_KPROBE = 1,
	/**
	 * @DNSK_HOOK_SYSCALL: replace the sys_call_table slot of @syscall_nr.
	 *
	 * @replacement is the new syscall body, @original_out receives the
	 * slot value that was there before.  Fails with -EOPNOTSUPP when the
	 * patching backend is unavailable, and the slot must not be NULL.
	 */
	DNSK_HOOK_SYSCALL,
	/**
	 * @DNSK_HOOK_DATA: replace a pointer stored at @target.
	 *
	 * @target points at the pointer slot to overwrite, @replacement is the
	 * new value.  The old value, which may legitimately be NULL, is
	 * recorded and written back by dnsk_hook_del().
	 */
	DNSK_HOOK_DATA,
	/**
	 * @DNSK_HOOK_KRETPROBE: kretprobe on @symbol.
	 *
	 * @target is the entry handler and @replacement is the return handler,
	 * both typed ``int (*)(struct kretprobe_instance *, struct pt_regs *)``.
	 * The return value is read with regs_return_value() from the return
	 * handler's @regs.
	 */
	DNSK_HOOK_KRETPROBE,
};

/**
 * struct dnsk_hook_req - one hook registration request
 *
 * Passed to dnsk_hook_add(), which copies or records what it needs; the
 * strings must stay valid only for the duration of the call.  Which fields
 * are required depends on @type.  Core does not pin @owner, so the registering
 * module must remove every hook it added in module_exit(); the reference the
 * module holds on core through dnsk_core_get() keeps core itself alive until
 * then.
 */
struct dnsk_hook_req {
	/** @key: unique name of the hook, shorter than DNSK_HOOK_KEY_LEN. */
	const char *key;
	/** @type: which kind of hook to install. */
	enum dnsk_hook_type type;
	/**
	 * @owner: registering module, for diagnostics.
	 *
	 * Core does not pin it.  The owner must remove every hook in
	 * module_exit().
	 */
	struct module *owner;
	/**
	 * @symbol: kernel symbol name for %DNSK_HOOK_KPROBE and
	 * %DNSK_HOOK_KRETPROBE, unused otherwise.
	 */
	const char *symbol;
	/** @syscall_nr: sys_call_table index for %DNSK_HOOK_SYSCALL. */
	unsigned int syscall_nr;
	/**
	 * @target: kretprobe entry handler, or the pointer slot to patch for
	 * %DNSK_HOOK_DATA.
	 */
	void *target;
	/** @replacement: handler or value to install. */
	void *replacement;
	/**
	 * @original_out: optional out-parameter.
	 *
	 * Receives the value displaced by the hook; may be %NULL when the
	 * caller does not need it.
	 */
	void **original_out;
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

enum dnsk_rw_opcode {
	DNSK_RW_READ = 1,
	DNSK_RW_WRITE,
	DNSK_RW_XCHG,
	DNSK_RW_CAS,
	DNSK_RW_PROBE,
};

enum dnsk_rw_lock_type {
	DNSK_RW_LOCK_NONE = 0,
	DNSK_RW_LOCK_MUTEX,
	DNSK_RW_LOCK_SPIN_IRQSAVE,
};

/* Perform the operation while holding req->lock. */
#define DNSK_RW_F_LOCKED (1u << 0)
/* The target is executable text: flush the instruction cache after writing. */
#define DNSK_RW_F_TEXT   (1u << 1)

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
	 * @hook_add: install a hook owned by core.
	 *
	 * @req: the hook to install.
	 *
	 * Return: %0 on success, negative errno on failure, including -EINVAL
	 * for a malformed request, -EBUSY for a duplicate @key, -ENOSPC when
	 * the registry is full, and -EOPNOTSUPP when the patching backend is
	 * not available.
	 */
	int (*hook_add)(const struct dnsk_hook_req *req);
	/**
	 * @hook_del: remove a hook by key and restore what it replaced.
	 *
	 * @key: the key the hook was added with.
	 *
	 * Restores whatever the hook displaced, then waits for an RCU grace
	 * period on success, so a caller may free its handler data on return.
	 *
	 * Return: %0 on success, -EINVAL for a %NULL @key, -ENOENT when no
	 * hook has that key, or the error from restoring the original.
	 */
	int (*hook_del)(const char *key);
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
