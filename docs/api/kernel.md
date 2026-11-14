# Kernel API

本页由 `make docs` 从 `core/include/*.h` 与 `droid-hook/include/*.h` 的 kernel-doc 注释自动生成, 不要手工编辑.

注释是唯一来源; 本页与头文件不一致时以头文件为准, 并重新生成.

## `DNSK_ABI_PROFILE`

build profile this module was compiled for

The build profile selects the ABI-sensitive config.  Every feature module stamps the profile and the macro-able knobs so dnskctl and package.py can reject a module built for a different profile.  The value is recorded in modinfo as @dnsk_abi_profile.  A module built without an explicit profile carries "unknown", which does not match any real profile and so fails those comparisons.

> 来源: `dnsk_abi.h` 的 kernel-doc 注释

## `DNSK_CORE_MAGIC`

identifies a valid struct dnsk_core_api

Feature modules compare this value against the @magic field before using any function pointer from the API, so that an unrelated struct returned by a stale or replaced core is rejected instead of dereferenced.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `DNSK_CORE_ABI`

ABI revision of struct dnsk_core_api

Passed by the feature module to dnsk_core_get().  The core refuses the request unless the caller's value matches exactly; the value changes whenever the layout or semantics of the API change.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `DNSK_FEATURE_NAME_LEN`

maximum feature name including the NUL

dnsk_feature_add() rejects a longer name with -ENAMETOOLONG.  Names are stored by value, the caller's string need not stay alive.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `DNSK_HOOK_KEY_LEN`

maximum hook key including the NUL

A key at or above this length is rejected with -EINVAL by dnsk_hook_add(). The key is copied into the hook registry and is what dnsk_hook_del() later uses to identify the hook.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_status`

implementation level reported for a feature

The level a feature module claims in `struct` dnsk_feature_desc.  It is informational: core stores it and prints it through the diag interface, it does not change how the feature behaves.  The single letters in the field descriptions match the levels defined in the project status document.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_hook_type`

kind of hook requested from the core

Selects which fields of `struct` dnsk_hook_req are meaningful and how core installs and removes the hook.  Core owns every hook it installs, so a feature module cannot leave a permanent hook behind on unload.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_hook_req`

one hook registration request

Passed to dnsk_hook_add(), which copies or records what it needs; the strings must stay valid only for the duration of the call.  Which fields are required depends on @type.  Core does not pin @owner, so the registering module must remove every hook it added in module_exit(); the reference the module holds on core through dnsk_core_get() keeps core itself alive until then.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_feature_desc`

one feature registration request

Passed to dnsk_feature_add(), which copies every string; the caller's buffers do not need to outlive the call.  Registrations are informational, they do not enable or install anything.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_rw_opcode`

operation selected by struct dnsk_rw_req

READ and WRITE copy bytes; XCHG and CAS are atomic read-modify-write; PROBE resolves src as a symbol name and stores the address in dst.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_rw_lock_type`

lock core takes when DNSK_RW_F_LOCKED is set

The caller must not already hold that lock; lock-held contexts use the lockless data path instead.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `DNSK_RW_F_LOCKED`

perform the operation while holding req->lock

The caller must not already hold that lock; for lock-held contexts use the lockless data path.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `DNSK_RW_F_TEXT`

the target is executable text

Flush the instruction cache after writing.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `DNSK_RW_F_RO`

the target may be read-only

Force the patch path, which sleeps and must not be used while holding a spinlock.  Without this flag XCHG/CAS/WRITE use hardware atomics or a direct store.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_rw_req`

one kernel read, write, exchange or probe

One request describes one operation.  The common case has no extra cost: READ and WRITE go straight to the copy or patch path, and no global lock is taken unless DNSK_RW_F_LOCKED is set.  @ret receives the result so callers can batch several requests through one dispatch.

Addresses are discovered with DNSK_RW_PROBE, which resolves @src as a symbol name and stores the address in @dst.  Code must not hard-code per-device addresses.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_core_api`

function table returned by dnsk_core_get()

The core fills this once at init and hands out a pointer to it.  A caller must validate @magic and @abi before dereferencing any function pointer; dnsk_core_get() already does both, so they are only needed if the structure is reached by other means.

> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `dnsk_core_get()`

obtain the core API function table

- `abi` ABI revision the caller was built against, normally DNSK_CORE_ABI.
- `size` size of the caller's `struct` dnsk_core_api, normally sizeof().
Validates the caller against the running core and, on success, takes a reference on the core module so the table stays valid while it is in use. Release that reference with module_put() on the returned @owner once the API is no longer needed.

Context: Process context.  May be called from module_init().

Return: the API table on success, or an ERR_PTR() value: -ENODEV when the core is not ready, -EPROTO when @abi or @size does not match.


> 来源: `dnsk_core.h` 的 kernel-doc 注释

## `DNSK_XT_LAYOUT`

xtables struct layout the module was built for

The value selects which xt_match/xt_target layout the assertions below accept: 104 when the target kernel was built without CONFIG_NETFILTER_XTABLES_COMPAT, 128 when it keeps the option enabled.  It defaults from DNSK_XT_NOCOMPAT, which the build profile sets, and may be supplied directly by the build; either way the two #error checks below reject an inconsistent pair.  The module also stamps the value as dnsk_xt_layout= so dnskctl and package.py can refuse a module built for the other layout before insmod.

The value is a property of the kernel's CONFIG, not of the KMI: a stock GKI/DDK 6.12 build is 128, while the MTK 6.12.30 device kernel is 104. Targeting another KMI or config means selecting the matching profile (and extending the build plus the preflight check for a third layout), not editing this macro.

> 来源: `dnsk_xt_layout.h` 的 kernel-doc 注释

## `DH_ABI`

Droid-Hook library ABI revision

Bumped whenever struct dh_backend, struct dh_ctx or a public signature changes.  dh_ctx_init() rejects a backend whose abi does not match.

> 来源: `dh.h` 的 kernel-doc 注释

## `DH_KEY_LEN`

maximum hook key including the NUL

Shared with the legacy dnsk_core hook registry so keys are interchangeable.

> 来源: `dh.h` 的 kernel-doc 注释

## `DH_UNDO_SLOTS`

number of undo entries in one transaction

Fixed and statically allocated so a rollback in a panic path never allocates.  dh_txn_record() returns -ENOSPC when the log is full.

> 来源: `dh.h` 的 kernel-doc 注释

## `DH_MAX_BATCH`

maximum instructions in one text-patch batch

One stop_machine window amortises the whole batch; larger sets must be split into multiple transactions.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_route`

how a read-only target is written

The route is chosen per transaction step.  ALIAS is preferred because it leaves the original PTE untouched; PTE is the KernelPatch hotpatch path; FIXMAP mirrors arm64 patch_map(); DMA is the EL2/Stage-2 fallback from the prior-art survey.  Each has the same post-write contract: dsb(ish), icache flush when the target is executable, TLB flush, read-back verify.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_ret`

unified return codes

Negative values are errnos so callers can propagate them unchanged. DH_E_VERIFY means the write was issued but the read-back did not match; the transaction must be rolled back, never treated as success.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_rw_req`

one backend memory operation

Mirrors the shape of struct dnsk_rw_req but is owned by Droid-Hook so the library never includes a core header.  The backend adapts this to whatever primitive the runtime provides.  @len is capped at one page by the runtime; the library splits larger transfers itself.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_backend`

runtime-provided primitives

The runtime fills this once and hands it to dh_ctx_init().  Every function pointer is mandatory except @alloc/@free, which may be NULL when the library only uses caller-supplied buffers.  @text_write must sleep-safe (stop_machine + fixmap); @rw must be callable from lock-held context for the data path.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_undo_ent`

one recorded mutation for rollback

An undo entry stores the original bytes, not a description of the write, so rollback is a plain reverse replay.  @route is recorded because the undo must use the same route that made the change.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_txn`

one atomic patch transaction

dh_txn_begin() snapshots nothing; each dh_txn_record() copies @len bytes from @addr before the caller writes.  dh_txn_commit() clears the undo log after read-back verification; dh_txn_rollback() replays it in reverse inside one quiesce window.  The log is intentionally fixed-size and statically allocated so a panic path never needs to allocate.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_ctx`

library instance

One per embedding module.  @be is borrowed, not copied: the runtime must outlive every ctx that references it.  @key namespace is shared with the legacy dnsk_core hook registry so the two cannot install a hook at the same address; @addr_arbitrate is called by dh_hook_install() to enforce it.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_hook`

one installed function-level hook

@trampoline points at the relo buffer (original instructions relocated plus a jump back to original+4-insn).  @chain is the before/after callback list, before in registration order, after in reverse.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_shadow_item`

one <obj,id> side-table entry

RCU-protected in the kernel build.  The object address is the key, so the parent struct layout never changes; this is the klp_shadow/KPM task_ext pattern from the design evidence.

> 来源: `dh.h` 的 kernel-doc 注释

## `dh_ctx_init()`

bind a library instance to a runtime backend

- `ctx` caller-owned context, zeroed before the call
- `be` runtime backend, must outlive @ctx
- `owner` stable name for diagnostics, copied into @ctx
Return: `DH_OK`, or `DH_E_INVAL` when @be lacks a mandatory primitive.


> 来源: `dh.h` 的 kernel-doc 注释

## `dh_ctx_fini()`

quiesce, roll back any open transaction, release ctx

- `ctx` context previously passed to dh_ctx_init()
Return: `DH_OK` on success, `DH_E_QUIESCE` when the runtime could not quiesce in-flight callers; the caller must retry, not force-unload.


> 来源: `dh.h` 的 kernel-doc 注释

## 覆盖检查

本次共生成 33 个带注释的公开声明, 覆盖审计通过.
