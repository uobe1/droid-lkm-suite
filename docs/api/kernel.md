# Kernel API

本页由 `make docs` 从 `core/include/*.h` 的 kernel-doc 注释自动生成, 不要手工编辑.

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

Selects which fields of `struct` dnsk_hook_req are meaningful and how core installs and removes the hook.  Core removes a hook only when dnsk_hook_del() is called with its key; a feature that skips the delete leaves a registry entry pointing at freed module text (use-after-free).

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

## 覆盖检查

本次共生成 18 个带注释的公开声明, 覆盖审计通过.
