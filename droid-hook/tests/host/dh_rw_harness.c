/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Pure-host mock backend for the Droid-Hook RW / transaction selftest.
 *
 * The mock owns a flat byte array as "kernel memory".  It implements the
 * dh_backend single-entry rw() with READ/WRITE/XCHG/CAS/PROBE, plus text/ro
 * writes, flush hooks and a quiesce counter.  Fault injection is deliberately
 * simple and observable from Python through exported accessors:
 *
 *   - corrupt_next_write: after a successful WRITE, flip one byte in the
 *     backing store, so dh_write()'s read-back verification must return
 *     DH_E_VERIFY.
 *   - fail_rw_ret: make rw() return this errno instead of performing the op.
 *   - fail_ro_write_ret: make ro_write() return this errno (rollback failure).
 *
 * No kernel headers, no malloc in the mock backend path that the library
 * rolls back through, and no global state outside this file's statics.
 */

#include "dh.h"

#include <stdio.h>
#include <string.h>

#define H_MEM_SIZE 4096
#define H_NR_SYMS 4

static unsigned char h_mem[H_MEM_SIZE];
static unsigned long h_quiesce_depth;
static unsigned long h_quiesce_calls;
static unsigned long h_ro_write_calls;
static int h_corrupt_next_write;
static int h_fail_rw_ret;
static int h_fail_ro_write_ret;
static int h_scratch[256];

static const char *h_sym_names[H_NR_SYMS] = {
	"cgroup_subsys", "sys_call_table", "init_mm", "_stext"
};
static void *h_sym_addrs[H_NR_SYMS];

/* Opcode numbers must match dh_rw.c's out-of-band contract. */
#define H_OP_READ  1
#define H_OP_WRITE 2
#define H_OP_XCHG  3
#define H_OP_CAS   4
#define H_OP_PROBE 5

static int h_ro_write(void *addr, const void *src, size_t len,
		      enum dh_route route);

static void h_init_syms(void)
{
	int i;

	for (i = 0; i < H_NR_SYMS; i++)
		h_sym_addrs[i] = (void *)(h_mem + 64 + i * 16);
}

static int h_rw(struct dh_rw_req *req)
{
	unsigned long off;
	size_t i;

	if (h_fail_rw_ret)
		return h_fail_rw_ret;
	if (!req || !req->addr || !req->len || req->len > H_MEM_SIZE)
		return DH_E_INVAL;

	off = (unsigned long)req->addr - (unsigned long)h_mem;
	if (off >= H_MEM_SIZE || off + req->len > H_MEM_SIZE)
		return DH_E_INVAL;

	switch (req->opcode) {
	case H_OP_READ:
		if (!req->dst)
			return DH_E_INVAL;
		memcpy(req->dst, h_mem + off, req->len);
		req->ret = DH_OK;
		return DH_OK;
	case H_OP_WRITE:
		if (!req->src)
			return DH_E_INVAL;
		memcpy(h_mem + off, req->src, req->len);
		req->ret = DH_OK;
		if (h_corrupt_next_write) {
			/* Simulate a write that landed but did not stick. */
			h_mem[off] ^= 0xA5;
			h_corrupt_next_write = 0;
		}
		return DH_OK;
	case H_OP_XCHG:
		if (req->len != 8 || (off & 7UL))
			return DH_E_INVAL;
		{
			u64 old;

			memcpy(&old, h_mem + off, sizeof(old));
			memcpy(h_mem + off, &req->value, sizeof(req->value));
			req->old = old;
			req->ret = DH_OK;
		}
		return DH_OK;
	case H_OP_CAS:
		if (req->len != 8 || (off & 7UL))
			return DH_E_INVAL;
		{
			u64 cur;

			memcpy(&cur, h_mem + off, sizeof(cur));
			if (cur != req->old)
				return DH_E_BUSY;
			memcpy(h_mem + off, &req->value, sizeof(req->value));
			req->ret = DH_OK;
		}
		return DH_OK;
	case H_OP_PROBE:
		if (!req->src || !req->dst)
			return DH_E_INVAL;
		for (i = 0; i < H_NR_SYMS; i++) {
			if (strcmp((const char *)req->src, h_sym_names[i]) == 0) {
				void **out = (void **)req->dst;

				*out = h_sym_addrs[i];
				req->ret = DH_OK;
				return DH_OK;
			}
		}
		return DH_E_NOENT;
	default:
		return DH_E_OPNOTSUPP;
	}
}

static int h_patch_pointer(void *target, void *value)
{
	unsigned long off = (unsigned long)target - (unsigned long)h_mem;

	if (off >= H_MEM_SIZE || off + sizeof(value) > H_MEM_SIZE)
		return DH_E_INVAL;
	memcpy(h_mem + off, &value, sizeof(value));
	return DH_OK;
}

static int h_text_write(void *addr, const void *src, size_t len,
			enum dh_route route)
{
	(void)route;
	return h_ro_write(addr, src, len, route);
}

static int h_ro_write(void *addr, const void *src, size_t len,
		      enum dh_route route)
{
	unsigned long off = (unsigned long)addr - (unsigned long)h_mem;

	(void)route;
	h_ro_write_calls++;
	if (h_fail_ro_write_ret)
		return h_fail_ro_write_ret;
	if (!src || off >= H_MEM_SIZE || off + len > H_MEM_SIZE)
		return DH_E_INVAL;
	memcpy(h_mem + off, src, len);
	return DH_OK;
}

static void *h_probe(const char *name)
{
	size_t i;

	for (i = 0; i < H_NR_SYMS; i++) {
		if (strcmp(name, h_sym_names[i]) == 0)
			return h_sym_addrs[i];
	}
	return NULL;
}

static void *h_alloc(size_t size)
{
	if (size > sizeof(h_scratch))
		return NULL;
	return h_scratch;
}

static void h_free(const void *p)
{
	(void)p;
}

static void h_icache_flush(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

static void h_tlb_flush(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

static void h_quiesce_begin(void)
{
	h_quiesce_depth++;
	h_quiesce_calls++;
}

static void h_quiesce_end(void)
{
	if (h_quiesce_depth)
		h_quiesce_depth--;
}

static struct dh_backend h_backend = {
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
	.name = "host-mock",
	.abi = DH_ABI,
};

/* ---- Python-facing accessors (plain C ABI, no pybind) ---- */

void dh_harness_reset(void)
{
	memset(h_mem, 0, sizeof(h_mem));
	h_quiesce_depth = 0;
	h_quiesce_calls = 0;
	h_ro_write_calls = 0;
	h_corrupt_next_write = 0;
	h_fail_rw_ret = 0;
	h_fail_ro_write_ret = 0;
	h_init_syms();
}

struct dh_backend *dh_harness_backend(void)
{
	return &h_backend;
}

unsigned char *dh_harness_mem(void)
{
	return h_mem;
}

unsigned long dh_harness_mem_size(void)
{
	return H_MEM_SIZE;
}

unsigned long dh_harness_quiesce_calls(void)
{
	return h_quiesce_calls;
}

unsigned long dh_harness_quiesce_depth(void)
{
	return h_quiesce_depth;
}

unsigned long dh_harness_ro_write_calls(void)
{
	return h_ro_write_calls;
}

void dh_harness_corrupt_next_write(void)
{
	h_corrupt_next_write = 1;
}

void dh_harness_fail_rw(int ret)
{
	h_fail_rw_ret = ret;
}

void dh_harness_fail_ro_write(int ret)
{
	h_fail_ro_write_ret = ret;
}

unsigned char dh_harness_read_byte(unsigned long off)
{
	return off < H_MEM_SIZE ? h_mem[off] : 0;
}

void dh_harness_write_byte(unsigned long off, unsigned char val)
{
	if (off < H_MEM_SIZE)
		h_mem[off] = val;
}

/* ---- High-level actions: each one drives the real library entry points ---- */

#define H_ACT_BUF 32

static struct dh_ctx h_ctx;

int dh_harness_init(void)
{
	return dh_ctx_init(&h_ctx, &h_backend, "host-selftest");
}

int dh_harness_fini(void)
{
	return dh_ctx_fini(&h_ctx);
}

int dh_harness_write(unsigned long off, const unsigned char *src, size_t len)
{
	if (off >= H_MEM_SIZE || off + len > H_MEM_SIZE)
		return DH_E_INVAL;
	return dh_write(&h_ctx, h_mem + off, src, len);
}

int dh_harness_read(unsigned long off, unsigned char *dst, size_t len)
{
	if (off >= H_MEM_SIZE || off + len > H_MEM_SIZE)
		return DH_E_INVAL;
	return dh_read(&h_ctx, h_mem + off, dst, len);
}

int dh_harness_xchg(unsigned long off, u64 value, u64 *old)
{
	if (off >= H_MEM_SIZE)
		return DH_E_INVAL;
	return dh_xchg64(&h_ctx, h_mem + off, value, old);
}

int dh_harness_cas(unsigned long off, u64 expected, u64 value)
{
	if (off >= H_MEM_SIZE)
		return DH_E_INVAL;
	return dh_cas64(&h_ctx, h_mem + off, expected, value);
}

void *dh_harness_probe(const char *name)
{
	return dh_probe(&h_ctx, name);
}

int dh_harness_txn_begin(void)
{
	return dh_txn_begin(&h_ctx);
}

int dh_harness_txn_record(unsigned long off, size_t len, int route)
{
	if (off >= H_MEM_SIZE || off + len > H_MEM_SIZE)
		return DH_E_INVAL;
	return dh_txn_record(&h_ctx, h_mem + off, len,
			     (enum dh_route)route);
}

int dh_harness_txn_commit(void)
{
	return dh_txn_commit(&h_ctx);
}

int dh_harness_txn_rollback(void)
{
	return dh_txn_rollback(&h_ctx);
}

u32 dh_harness_txn_n(void)
{
	return h_ctx.txn.n;
}

int dh_harness_txn_active(void)
{
	return (int)h_ctx.txn.active;
}

int dh_harness_txn_failed(void)
{
	return (int)h_ctx.txn.failed;
}

void dh_harness_set_backend_rw_null(void)
{
	struct dh_backend save = h_backend;

	h_backend.rw = NULL;
	/* Call dh_ctx_init with the broken backend to observe DH_E_INVAL,
	 * then restore without disturbing the live ctx. */
	{
		struct dh_ctx probe_ctx;

		(void)dh_ctx_init(&probe_ctx, &h_backend, "probe");
	}
	h_backend = save;
}

int dh_harness_init_with_null_rw(void)
{
	struct dh_backend broken = h_backend;
	struct dh_ctx probe_ctx;

	broken.rw = NULL;
	return dh_ctx_init(&probe_ctx, &broken, "probe");
}
