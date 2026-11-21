/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DROID_HOOK_RUNTIME_DH_CORE_TYPES_H
#define _DROID_HOOK_RUNTIME_DH_CORE_TYPES_H

/*
 * Core ABI 5 types are visible only inside runtime/.  The library contract
 * (dh.h) and src/ must never include a core header.
 *
 * In a real kernel build we use the authoritative core/include/dnsk_core.h.
 * Host builds have no kernel headers, so this header supplies the byte-for-byte
 * equivalent declarations used by the selftest harness.  The two must stay in
 * sync; dh_core_backend.c only ever uses the fields below, never a private
 * offset or a guessed signature.
 */

#ifdef __KERNEL__
#include <dnsk_core.h>
#else

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define DNSK_CORE_MAGIC 0x444E534BU
#define DNSK_CORE_ABI 5

#define DNSK_RW_F_LOCKED (1u << 0)
#define DNSK_RW_F_TEXT   (1u << 1)
#define DNSK_RW_F_RO     (1u << 2)

#define DNSK_RW_MAX 256

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
	DNSK_RW_LOCK_RWSEM_READ,
};

struct dnsk_rw_req {
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

struct module;

struct dnsk_core_api {
	u32 magic;
	u16 abi;
	u16 size;
	void *(*resolve)(const char *name);
	int (*hook_add)(const void *req);
	int (*hook_del)(const char *key);
	int (*feature_add)(const void *desc);
	int (*feature_del)(const char *name);
	void (*log)(int level, const char *fmt, ...);
	int (*patch_pointer)(void *target, void *value);
	int (*rw)(struct dnsk_rw_req *req);
	struct module *owner;
};

#endif /* __KERNEL__ */

#endif /* _DROID_HOOK_RUNTIME_DH_CORE_TYPES_H */
