/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host harness for the core RW ABI 5 -> dh_backend adapter.
 *
 * Provides a mock struct dnsk_core_api (dh_core_types.h supplies the same
 * declarations in a host build, so no kernel header is needed).  The mock
 * records every rw() call, can inject a failure at a chosen call index, and
 * prints machine-readable lines for the Python unittest to assert.
 *
 * Build: cc -std=c11 -Wall -Wextra -Werror \
 *          -Idroid-hook/include -Idroid-hook/runtime \
 *          dh_runtime_harness.c ../runtime/dh_core_backend.c -o harness
 *
 * Usage: ./harness <scenario>
 */

#include "dh.h"
#include "dh_core_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mock memory the "kernel" reads/writes.
 *
 * The pool is page-aligned so its page offset is deterministic, but the
 * pointer handed to the adapter can be shifted by DH_MOCK_SHIFT bytes at
 * compile time.  That reproduces the linker-dependent base offsets seen in
 * the field (0, 64 and 96 mod 256) on demand, so the split assertions can be
 * proven alignment-independent instead of assuming one layout.
 */
#define MOCK_MEM_SIZE 4096
#define MOCK_POOL_SIZE (MOCK_MEM_SIZE + 512)
#ifndef DH_MOCK_SHIFT
#define DH_MOCK_SHIFT 0
#endif
static _Alignas(4096) u8 mock_pool[MOCK_POOL_SIZE];
static u8 *mock_mem = mock_pool + DH_MOCK_SHIFT;
_Static_assert(DH_MOCK_SHIFT >= 0 && DH_MOCK_SHIFT < 512,
	       "DH_MOCK_SHIFT must stay inside the pool slack");
static u8 scratch[MOCK_MEM_SIZE];

/* Call log. */
#define MAX_LOG 128
struct call_log {
	u16 opcode;
	u16 flags;
	size_t len;
	size_t off;
	int ret;
};
static struct call_log log[MAX_LOG];
static int nr_log;
static int fail_at = -1;      /* inject failure at this 1-based call index */
static int fail_errno = -5;   /* EIO-ish, distinct from DH_E_* */

static void reset_mock(void)
{
	memset(mock_mem, 0xA5, MOCK_MEM_SIZE);
	memset(scratch, 0, sizeof(scratch));
	memset(log, 0, sizeof(log));
	nr_log = 0;
	fail_at = -1;
	fail_errno = -5;
}

static int record_call(struct dnsk_rw_req *req)
{
	int idx;

	if (nr_log < MAX_LOG) {
		idx = nr_log;
		log[idx].opcode = req->opcode;
		log[idx].flags = req->flags;
		log[idx].len = req->len;
		log[idx].off = req->addr ? (size_t)((u8 *)req->addr - mock_mem) : 0;
		log[idx].ret = req->ret;
		nr_log++;
	}
	if (fail_at > 0 && nr_log == fail_at) {
		req->ret = fail_errno;
		return 0;
	}
	return 0;
}

static int mock_rw(struct dnsk_rw_req *req)
{
	size_t n;
	const u8 *src;
	u8 *dst;

	(void)record_call(req);
	if (req->ret != 0) {
		if (nr_log > 0)
			log[nr_log - 1].ret = req->ret;
		return 0;
	}

	switch (req->opcode) {
	case DNSK_RW_READ:
		if (req->addr == NULL || req->dst == NULL)
			return -1;
		n = req->len;
		memcpy(req->dst, req->addr, n);
		req->ret = 0;
		break;
	case DNSK_RW_WRITE:
		if (req->addr == NULL || req->src == NULL)
			return -1;
		src = req->src;
		dst = req->addr;
		n = req->len;
		memcpy(dst, src, n);
		req->ret = 0;
		break;
	case DNSK_RW_XCHG:
		req->old = *(u64 *)req->addr;
		*(u64 *)req->addr = req->value;
		req->ret = 0;
		break;
	case DNSK_RW_CAS:
		if (*(u64 *)req->addr == req->old) {
			*(u64 *)req->addr = req->value;
			req->ret = 0;
		} else {
			req->ret = -16;
		}
		break;
	case DNSK_RW_PROBE:
		/* src is the symbol name; dst receives void**. */
		if (req->dst != NULL)
			*(void **)req->dst = (void *)(uintptr_t)0xDEADBEEF;
		req->ret = 0;
		break;
	default:
		req->ret = -22;
		break;
	}
	/* The log records the effective ret after the operation. */
	if (nr_log > 0)
		log[nr_log - 1].ret = req->ret;
	return 0;
}

static void *mock_resolve(const char *name)
{
	if (name != NULL && strcmp(name, "cgroup_subsys") == 0)
		return (void *)(uintptr_t)0xCAFEBABE;
	return NULL;
}

static int mock_patch_pointer(void *target, void *value)
{
	if (target == NULL)
		return -22;
	*(void **)target = value;
	return 0;
}

static void mock_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

static struct dnsk_core_api api;
static struct dh_backend be;
static struct dh_core_state st;

static void api_reset(void)
{
	memset(&api, 0, sizeof(api));
	api.magic = DNSK_CORE_MAGIC;
	api.abi = DNSK_CORE_ABI;
	api.size = (u16)sizeof(api);
	api.rw = mock_rw;
	api.resolve = mock_resolve;
	api.patch_pointer = mock_patch_pointer;
	api.log = mock_log;
}

static void dump_log(void)
{
	int i;

	printf("CALLS=%d\n", nr_log);
	for (i = 0; i < nr_log; i++)
		printf("CALL %d opcode=%u flags=%u len=%zu off=%zu ret=%d\n",
		       i + 1, log[i].opcode, log[i].flags, log[i].len,
		       log[i].off, log[i].ret);
}

static void dump_backend(void)
{
	printf("BACKEND name=%s abi=%u rw=%p text=%p ro=%p probe=%p pp=%p\n",
	       be.name ? be.name : "(null)", be.abi,
	       (void *)(uintptr_t)be.rw, (void *)(uintptr_t)be.text_write,
	       (void *)(uintptr_t)be.ro_write, (void *)(uintptr_t)be.probe,
	       (void *)(uintptr_t)be.patch_pointer);
}

static int scenario_init(void)
{
	reset_mock();
	api_reset();
	memset(&be, 0, sizeof(be));
	memset(&st, 0, sizeof(st));
	printf("INIT=%d\n", dh_core_backend_init(&be, &st, &api));
	dump_backend();
	printf("PAGE=%zu\n", dh_core_backend_page_size(&st));
	printf("CHUNK_MAX=%d\n", DNSK_RW_MAX);
	return 0;
}

static int scenario_bad(const char *what)
{
	reset_mock();
	api_reset();
	memset(&be, 0, sizeof(be));
	memset(&st, 0, sizeof(st));

	if (strcmp(what, "null") == 0)
		printf("INIT=%d\n", dh_core_backend_init(&be, &st, NULL));
	else if (strcmp(what, "magic") == 0) {
		api.magic = 0x12345678U;
		printf("INIT=%d\n", dh_core_backend_init(&be, &st, &api));
	} else if (strcmp(what, "abi") == 0) {
		api.abi = 4;
		printf("INIT=%d\n", dh_core_backend_init(&be, &st, &api));
	} else if (strcmp(what, "norw") == 0) {
		api.rw = NULL;
		printf("INIT=%d\n", dh_core_backend_init(&be, &st, &api));
	} else {
		return 2;
	}
	dump_backend();
	return 0;
}

static int scenario_data(const char *op)
{
	struct dh_rw_req req;
	u64 old = 0;
	u64 val = 0x1122334455667788ULL;
	u64 dst = 0;
	int rc;

	reset_mock();
	api_reset();
	memset(&be, 0, sizeof(be));
	memset(&st, 0, sizeof(st));
	if (dh_core_backend_init(&be, &st, &api) != DH_OK)
		return 3;

	memset(&req, 0, sizeof(req));
	if (strcmp(op, "read") == 0) {
		req.opcode = DNSK_RW_READ;
		req.addr = mock_mem + 16;
		req.dst = &dst;
		req.len = 8;
	} else if (strcmp(op, "write") == 0) {
		req.opcode = DNSK_RW_WRITE;
		req.addr = mock_mem + 32;
		req.src = &val;
		req.len = 8;
	} else if (strcmp(op, "xchg") == 0) {
		*(u64 *)(mock_mem + 48) = 0x99;
		req.opcode = DNSK_RW_XCHG;
		req.addr = mock_mem + 48;
		req.value = val;
	} else if (strcmp(op, "cas") == 0) {
		*(u64 *)(mock_mem + 64) = 0x99;
		req.opcode = DNSK_RW_CAS;
		req.addr = mock_mem + 64;
		req.old = 0x99;
		req.value = val;
	} else {
		return 2;
	}
	rc = be.rw(&req);
	printf("RC=%d\n", rc);
	printf("OLD=%llu\n", (unsigned long long)req.old);
	printf("MEM48=%llu\n", (unsigned long long)*(u64 *)(mock_mem + 48));
	printf("MEM64=%llu\n", (unsigned long long)*(u64 *)(mock_mem + 64));
	printf("MEM16=%llu\n", (unsigned long long)*(u64 *)(mock_mem + 16));
	dump_log();
	(void)old;
	return 0;
}

static int scenario_flags(const char *which)
{
	int rc;

	reset_mock();
	api_reset();
	memset(&be, 0, sizeof(be));
	memset(&st, 0, sizeof(st));
	if (dh_core_backend_init(&be, &st, &api) != DH_OK)
		return 3;

	if (strcmp(which, "text") == 0)
		rc = be.text_write(mock_mem + 128, scratch, 16, DH_ROUTE_FIXMAP);
	else
		rc = be.ro_write(mock_mem + 256, scratch, 16, DH_ROUTE_ALIAS);
	printf("RC=%d\n", rc);
	dump_log();
	return 0;
}

/* Parse an optional decimal/hex argument; dflt when absent or malformed. */
static long arg_long(const char *s, long dflt)
{
	char *end = NULL;
	long v;

	if (s == NULL)
		return dflt;
	v = strtol(s, &end, 0);
	if (end == s || *end != '\0')
		return dflt;
	return v;
}

/* split <mode> [start_off] [len] [fail_at]
 *
 * Prints everything the Python gate needs to derive the expected chunk
 * sequence instead of hardcoding it: PAGE, CHUNK_MAX, BASE_ALIGN (mock_mem
 * modulo the page), START and LEN.
 */
static int scenario_split(const char *mode, const char *a_off,
			  const char *a_len, const char *a_fail)
{
	struct dh_rw_req req;
	static u8 big[600];
	size_t i, page, sum, start_off, len;
	long start_l = arg_long(a_off, 8);
	long len_l = arg_long(a_len, (long)sizeof(big));
	long fail_l = arg_long(a_fail, -1);
	int rc;

	if (start_l < 0 || len_l <= 0 || (size_t)len_l > sizeof(big) ||
	    (size_t)start_l + (size_t)len_l > MOCK_MEM_SIZE)
		return 2;
	start_off = (size_t)start_l;
	len = (size_t)len_l;

	reset_mock();
	api_reset();
	memset(&be, 0, sizeof(be));
	memset(&st, 0, sizeof(st));
	if (dh_core_backend_init(&be, &st, &api) != DH_OK)
		return 3;
	for (i = 0; i < len; i++)
		big[i] = (u8)(i & 0xff);

	page = dh_core_backend_page_size(&st);
	printf("PAGE=%zu\n", page);
	printf("CHUNK_MAX=%d\n", DNSK_RW_MAX);
	printf("BASE_ALIGN=%zu\n",
	       (size_t)((unsigned long)mock_mem & (page - 1)));
	printf("START=%zu LEN=%zu\n", start_off, len);

	memset(&req, 0, sizeof(req));
	req.opcode = strcmp(mode, "read") == 0 ? DNSK_RW_READ : DNSK_RW_WRITE;
	req.addr = mock_mem + start_off;
	req.src = big;
	req.dst = scratch;
	req.len = len;

	if (fail_l > 0)
		fail_at = (int)fail_l;
	else if (strcmp(mode, "fail") == 0)
		fail_at = 3;      /* legacy default: fail during save_originals */
	else if (strcmp(mode, "fail_write") == 0)
		fail_at = 5;      /* legacy default: fail on the 2nd WRITE */

	rc = be.rw(&req);
	printf("RC=%d\n", rc);
	printf("MEM_FIRST=%u\n", (unsigned)mock_mem[start_off]);
	printf("MEM_LAST=%u\n", (unsigned)mock_mem[start_off + len - 1]);
	printf("MEM8=%u\n", (unsigned)mock_mem[8]);
	printf("MEM263=%u\n", (unsigned)mock_mem[263]);
	sum = 0;
	for (i = 0; i < len; i++)
		sum += mock_mem[start_off + i];
	printf("MEM_SUM=%lu\n", (unsigned long)sum);
	printf("SCRATCH0=%u\n", (unsigned)scratch[0]);
	dump_log();
	return 0;
}

static int scenario_probe(void)
{
	void *p;

	reset_mock();
	api_reset();
	memset(&be, 0, sizeof(be));
	memset(&st, 0, sizeof(st));
	if (dh_core_backend_init(&be, &st, &api) != DH_OK)
		return 3;
	p = be.probe("cgroup_subsys");
	printf("PROBE=%p\n", p);
	p = be.probe("no_such_symbol");
	printf("PROBE_MISS=%p\n", p);
	return 0;
}

static int scenario_patch_pointer(void)
{
	void *slot = NULL;
	int rc;

	reset_mock();
	api_reset();
	memset(&be, 0, sizeof(be));
	memset(&st, 0, sizeof(st));
	if (dh_core_backend_init(&be, &st, &api) != DH_OK)
		return 3;
	rc = be.patch_pointer(&slot, (void *)(uintptr_t)0x1234);
	printf("RC=%d SLOT=%p\n", rc, slot);
	return 0;
}

static int scenario_fini(void)
{
	reset_mock();
	api_reset();
	memset(&be, 0, sizeof(be));
	memset(&st, 0, sizeof(st));
	printf("INIT=%d\n", dh_core_backend_init(&be, &st, &api));
	printf("FINI=%d\n", dh_core_backend_fini(&be));
	printf("FINI2=%d\n", dh_core_backend_fini(&be));
	dump_backend();
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	if (strcmp(argv[1], "init") == 0)
		return scenario_init();
	if (strcmp(argv[1], "bad") == 0)
		return argc > 2 ? scenario_bad(argv[2]) : 2;
	if (strcmp(argv[1], "data") == 0)
		return argc > 2 ? scenario_data(argv[2]) : 2;
	if (strcmp(argv[1], "flags") == 0)
		return argc > 2 ? scenario_flags(argv[2]) : 2;
	if (strcmp(argv[1], "split") == 0)
		return argc > 2 ? scenario_split(argv[2],
						 argc > 3 ? argv[3] : NULL,
						 argc > 4 ? argv[4] : NULL,
						 argc > 5 ? argv[5] : NULL) : 2;
	if (strcmp(argv[1], "probe") == 0)
		return scenario_probe();
	if (strcmp(argv[1], "patch_pointer") == 0)
		return scenario_patch_pointer();
	if (strcmp(argv[1], "fini") == 0)
		return scenario_fini();
	return 2;
}
