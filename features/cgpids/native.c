// SPDX-License-Identifier: GPL-2.0-only
/*
 * Real pids cgroup_subsys injection into a compiled-in v1-only slot.
 *
 * CONFIG_CGROUP_PIDS=n removes pids from the compile-time cgroup_subsys[],
 * css_set->subsys[] and cgroup->subsys[] arrays; the arrays are exactly full.
 * The device Image compiles net_prio into slot 7 and never mounts it
 * (hierarchy 0 in /proc/cgroups, no v1 net_prio root), so this backend takes
 * that slot over: the kernel's own cgroup core then drives css creation,
 * migration, cftypes and fork/exit callbacks for a real pids subsystem.
 *
 * This is not a rebuilt kernel: single writes go through core's fixmap
 * patcher, the root css is installed by hand (the boot-only cgroup_init_subsys
 * is gone), and every callback below is a native pids implementation.  The
 * slot choice and struct offsets come from the ABI profile.  The module takes a
 * self reference while the slot is installed; dnsk_cgpids_native_quiesce()
 * hands the slot back and drops it, after which rmmod can proceed.
 */

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/cgroup.h>
#include <linux/cgroup-defs.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/kernfs.h>
#include <linux/kstrtox.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/threads.h>
#include <dnsk_abi.h>
#include <dnsk_core.h>
#include "cgpids.h"

#ifndef DNSK_CGROUP_SUBSYS_SLOT
#define DNSK_CGROUP_SUBSYS_SLOT -1
#endif
#ifndef DNSK_SEQ_FILE_PRIVATE
#define DNSK_SEQ_FILE_PRIVATE 128
#endif

#if DNSK_CGROUP_SUBSYS_SLOT < 0

int dnsk_cgpids_native_init(struct dnsk_core_api *api)
{
	return -EOPNOTSUPP;
}

int dnsk_cgpids_native_exit(void)
{
}

int dnsk_cgpids_native_quiesce(void)
{
	return 0;
}

bool dnsk_cgpids_native_active(void)
{
	return false;
}

#else

#define DNSK_PIDS_MAX (PID_MAX_LIMIT + 1ULL)
#define DNSK_PIDS_MAX_STR "max"
#define DNSK_CSS_SET_BUCKETS 128
#define DNSK_PIDCG_MAX 0
#define DNSK_PIDCG_FORKFAIL 1
#define DNSK_PIDCG_EVENTS 2

enum {
	DNSK_SS_CSS_ALLOC = DNSK_CGROUP_SUBSYS_CSS_ALLOC,
	DNSK_SS_CSS_FREE = DNSK_CGROUP_SUBSYS_CSS_FREE,
	DNSK_SS_CAN_ATTACH = DNSK_CGROUP_SUBSYS_CAN_ATTACH,
	DNSK_SS_CANCEL_ATTACH = DNSK_CGROUP_SUBSYS_CANCEL_ATTACH,
	DNSK_SS_CAN_FORK = DNSK_CGROUP_SUBSYS_CAN_FORK,
	DNSK_SS_CANCEL_FORK = DNSK_CGROUP_SUBSYS_CANCEL_FORK,
	DNSK_SS_RELEASE = DNSK_CGROUP_SUBSYS_RELEASE,
	DNSK_SS_ID = DNSK_CGROUP_SUBSYS_ID,
	DNSK_SS_NAME = DNSK_CGROUP_SUBSYS_NAME,
	DNSK_SS_LEGACY_NAME = DNSK_CGROUP_SUBSYS_LEGACY_NAME,
	DNSK_SS_ROOT = DNSK_CGROUP_SUBSYS_ROOT,
	DNSK_SS_CSS_IDR = DNSK_CGROUP_SUBSYS_CSS_IDR,
	DNSK_SS_CFTS = DNSK_CGROUP_SUBSYS_CFTS,
	DNSK_SS_DFL_CFTYPES = DNSK_CGROUP_SUBSYS_DFL_CFTYPES,
	DNSK_SS_LEGACY_CFTYPES = DNSK_CGROUP_SUBSYS_LEGACY_CFTYPES,
};

struct dnsk_pids_css {
	struct cgroup_subsys_state css;
	atomic64_t counter;
	atomic64_t limit;
	s64 watermark;
	struct cgroup_file events_file;
	struct cgroup_file events_local_file;
	atomic64_t events[DNSK_PIDCG_EVENTS];
	atomic64_t events_local[DNSK_PIDCG_EVENTS];
};

static u8 dnsk_ss[DNSK_CGROUP_SUBSYS_LAYOUT] __aligned(8);
int dnsk_cgpids_native_exit(void);

static struct dnsk_core_api *dnsk_native_api;
static struct cgroup_subsys **dnsk_subsys_array;
static struct cgroup_root *dnsk_root;
static struct css_set *dnsk_init_cset;
static struct static_key *dnsk_on_dfl_key;
static struct static_key_true **dnsk_on_dfl_keys;
static u16 *dnsk_inhibit_mask;
static u16 *dnsk_have_canfork;
static u16 *dnsk_have_release;
static spinlock_t *dnsk_css_set_lock;
static struct hlist_head *dnsk_css_set_table;
static struct cgroup_subsys_state *(*dnsk_native_of_css)(struct kernfs_open_file *of);
static int (*dnsk_add_dfl_cftypes)(struct cgroup_subsys *ss, struct cftype *cfts);
static void (*dnsk_static_key_enable)(struct static_key *key);
static void (*dnsk_static_key_disable)(struct static_key *key);
static void (*dnsk_cgroup_file_notify)(struct cgroup_file *cfile);
static bool dnsk_native_ready;
/* True while this module holds its own reference (see dnsk_cgpids_native_init). */
static bool dnsk_self_ref;
/* Saved at init so teardown can hand slot 7 back to net_prio unchallenged. */
static struct cgroup_subsys *dnsk_saved_ss;
static struct cgroup_subsys_state *dnsk_saved_root_css;
/* The root css this module installed.  Teardown must inspect *this* object:
 * after a failed or partial handback the slot may already hold the previous
 * subsystem again, and container_of()ing that pointer would read garbage. */
static struct cgroup_subsys_state *dnsk_own_root_css;
static u16 dnsk_saved_inhibit;
static bool dnsk_saved_key_was_enabled;
static u16 dnsk_saved_have_canfork;
static u16 dnsk_saved_have_release;

/* Read-only feasibility probe; enabled with dnsk_cgpids.rw_probe=1. */
static bool dnsk_rw_probe;
module_param_named(rw_probe, dnsk_rw_probe, bool, 0444);

static void dnsk_rwprobe_probe(const char *name)
{
	struct dnsk_rw_req req = {0};
	void *addr = NULL;

	req.opcode = DNSK_RW_PROBE;
	req.src = name;
	req.dst = &addr;
	req.ret = dnsk_native_api->rw(&req);
	if (req.ret)
		pr_info("dnsk_rwprobe: probe %s failed %d\n", name, req.ret);
	else
		pr_info("dnsk_rwprobe: %s=%px\n", name, addr);
}

/*
 * ===== Pure AArch64 reference decode/encode helpers =====================
 *
 * Everything between the BEGIN/END markers below is free of kernel
 * dependencies: plain integer arithmetic over instruction words.  The host
 * test tests/host/test_aarch64_refs.py extracts this exact text, compiles it
 * with the host cc and drives it through ctypes, so the unit tests exercise
 * the very code the module runs instead of a second, drifting copy.  Keep the
 * region self-contained: no READ_ONCE, no kernel helpers -- only u32/u64/s64/
 * bool and the enums/macros defined inside it.
 */
/* DNSK_AARCH64_PURE_BEGIN */
enum dnsk_ref_kind {
	DNSK_REF_NONE = 0,
	DNSK_REF_ADD,
	DNSK_REF_LDR,
};

enum dnsk_reloc_result {
	DNSK_RELOC_NONE = 0,  /* not an ADRP + ADD/LDR pair into the window */
	DNSK_RELOC_PATCH = 1, /* buf[0..1] hold the retargeted words */
	DNSK_RELOC_FAR = 2,   /* target matched but ADRP cannot reach new page */
};

/*
 * Bytes retargeted from the old array base.  The array is
 * CGROUP_SUBSYS_COUNT pointers (8 * 8 = 64 bytes on beryl), so 64 bytes
 * covers every element read through the old base.
 */
#define DNSK_RELOC_WINDOW 64

/* ADRP: immlo bits[30:29], immhi bits[23:5], a signed 21-bit page count. */
static inline bool dnsk_insn_is_adrp(u32 insn)
{
	return (insn & 0x9F000000u) == 0x90000000u;
}

static inline u64 dnsk_adrp_target_page(u64 pc, u32 insn)
{
	s64 imm = (s64)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));

	if (imm & (1 << 20))
		imm |= ~((s64)((1 << 21) - 1));
	return (pc & ~0xFFFULL) + ((u64)imm << 12);
}

/* Rewrite an ADRP to name a new page (same 21-bit signed page contract). */
static inline u32 dnsk_adrp_page(u32 insn, u64 pc, u64 page)
{
	s64 delta = ((s64)(page - (pc & ~0xFFFULL))) >> 12;
	u32 lo = (u32)(delta & 3);
	u32 hi = (u32)((delta >> 2) & 0x7FFFF);

	return (insn & ~0x60FFFFE0u) | (lo << 29) | (hi << 5);
}

static inline bool dnsk_insn_is_add_imm(u32 insn)
{
	return (insn & 0xFF800000u) == 0x91000000u;
}

/* LDR (immediate, unsigned offset), 64-bit variant (opc = 11). */
static inline bool dnsk_insn_is_ldr_uimm64(u32 insn)
{
	return (insn & 0xFFC00000u) == 0xF9400000u;
}

/* ADD (immediate): page + imm12 << (sh ? 12 : 0); sh is bit 22. */
static inline u64 dnsk_add_imm_target(u32 insn, u64 page)
{
	u64 imm = (insn >> 10) & 0xFFFu;
	u64 sh = (((insn >> 22) & 3u) == 1u) ? 12u : 0u;

	return page + (imm << sh);
}

/* LDR (unsigned offset, 64-bit): base + imm12 * 8. */
static inline u64 dnsk_ldr_uimm_target(u32 insn, u64 base)
{
	return base + ((insn >> 10) & 0xFFFu) * 8u;
}

/*
 * ADD (immediate) repack.  imm12 lives in bits[21:10] (mask 0x003FFC00); sh
 * is bit 22; bit 23 is the low bit of the fixed 0b100010 opcode and is always
 * 0 for ADD.  `off` is a byte page offset, so the shift must be 0: clear
 * imm12 and sh explicitly.  For every ADD this scan can match (sh == 0 and
 * bit23 == 0) this is byte-identical to the old, wider 0x00FFFC00 mask, but
 * the narrow imm12 mask plus the explicit sh clear states the intent and is
 * pinned by the unit test's mask/equivalence cases.
 */
static inline u32 dnsk_repack_add_imm(u32 insn, u32 off)
{
	return (insn & ~(0x003FFC00u | 0x00400000u)) | ((off & 0xFFFu) << 10);
}

/* LDR (unsigned offset, 64-bit) repack: imm12 bits[21:10], scaled by 8. */
static inline u32 dnsk_repack_ldr_uimm64(u32 insn, u32 off)
{
	return (insn & ~0x003FFC00u) | (((off >> 3) & 0xFFFu) << 10);
}

/* Classify an instruction pair as an ADRP + ADD/LDR reference. */
static inline enum dnsk_ref_kind dnsk_ref_pair(u64 pc, u32 i0, u32 i1, u64 *tgt)
{
	u64 pg;

	if (!dnsk_insn_is_adrp(i0))
		return DNSK_REF_NONE;
	pg = dnsk_adrp_target_page(pc, i0);
	if (dnsk_insn_is_add_imm(i1)) {
		*tgt = dnsk_add_imm_target(i1, pg);
		return DNSK_REF_ADD;
	}
	if (dnsk_insn_is_ldr_uimm64(i1)) {
		*tgt = dnsk_ldr_uimm_target(i1, pg);
		return DNSK_REF_LDR;
	}
	return DNSK_REF_NONE;
}

static inline bool dnsk_ref_in_window(u64 tgt, u64 base)
{
	return tgt >= base && tgt < base + DNSK_RELOC_WINDOW;
}

/*
 * Decide whether the pair at `pc` references the old array and, if so, put
 * the two retargeted words in buf[0]/buf[1].  Shared by dnsk_rwprobe_reloc()
 * and the host unit test.
 */
static inline enum dnsk_reloc_result dnsk_reloc_pair(u64 pc, u32 i0, u32 i1, u64 olda, u64 neua,
						     u32 buf[2])
{
	u64 tgt, na;
	s64 delta_pages;
	enum dnsk_ref_kind kind = dnsk_ref_pair(pc, i0, i1, &tgt);

	if (kind == DNSK_REF_NONE || !dnsk_ref_in_window(tgt, olda))
		return DNSK_RELOC_NONE;
	na = neua + (tgt - olda);
	delta_pages = (s64)((na & ~0xFFFULL) - (pc & ~0xFFFULL)) >> 12;
	if (delta_pages < -(1 << 20) || delta_pages >= (1 << 20))
		return DNSK_RELOC_FAR;
	buf[0] = dnsk_adrp_page(i0, pc, na & ~0xFFFULL);
	if (kind == DNSK_REF_ADD)
		buf[1] = dnsk_repack_add_imm(i1, (u32)(na & 0xFFF));
	else
		buf[1] = dnsk_repack_ldr_uimm64(i1, (u32)(na & 0xFFF));
	return DNSK_RELOC_PATCH;
}
/* DNSK_AARCH64_PURE_END */

static void dnsk_rwprobe_scan_refs(void *target)
{
	u64 addr = (u64)target;
	u64 tgt;
	u32 i0, i1, *p;
	int adrp_add = 0, adrp_ldr = 0;
	enum dnsk_ref_kind kind;
	void *stext = dnsk_native_api->resolve("_stext");
	void *etext = dnsk_native_api->resolve("_etext");

	if (!stext || !etext || etext <= stext)
		return;
	for (p = stext; (void *)(p + 2) <= etext; p++) {
		i0 = READ_ONCE(p[0]);
		i1 = READ_ONCE(p[1]);
		kind = dnsk_ref_pair((u64)p, i0, i1, &tgt);
		if (kind == DNSK_REF_NONE || !dnsk_ref_in_window(tgt, addr))
			continue;
		if (kind == DNSK_REF_ADD)
			adrp_add++;
		else
			adrp_ldr++;
	}
	pr_info("dnsk_rwprobe: refs add=%d ldr=%d range=%px-%px\n", adrp_add, adrp_ldr, stext,
		etext);
}

/*
 * Relocate cgroup_subsys[] to a same-content, one-entry-bigger array and
 * retarget every ADRP+ADD/LDR pair that referenced the old array.  Enabled
 * only with dnsk_cgpids.rw_reloc=1; the default (0) never calls this, so the
 * gated slot-7 injection path is untouched.
 *
 * WHAT THIS DOES NOT DO -- do not read `patched > 0` as "the 9th slot works":
 * it only rewrites *reference pairs* to the array base.  It does NOT rewrite
 * the CGROUP_SUBSYS_COUNT immediate the compiler folded into loop bounds,
 * `cmp`/`mov` operands and bitmasks, and it does not touch the shadow arrays
 * (cgroup_subsys_name[], *_enabled_key[], *_on_dfl_key[], and the embedded
 * css_set->subsys[] / cgroup->subsys[] / e_csets[] / nr_dying_subsys[]).
 * Because the copy has identical content, any missed reader still sees the
 * whole original table, so this is a *safe mechanism probe*, not a working
 * 9th slot.
 *
 * Why the CGROUP_SUBSYS_COUNT immediate rewrite is deliberately deferred and
 * not done here:
 *   - The count has no symbol (the device probe read `cgroup_subsys_count`
 *     as -ENOENT: it is fully folded), so there is no authoritative site
 *     list.  A literal `#8 -> #9` sweep over _stext.._etext matches countless
 *     unrelated 8s; patching the wrong immediate corrupts arbitrary code.
 *   - Patching only *some* bound sites is strictly worse than none: a loop
 *     widened to 9 would index a still-8-entry shadow array out of bounds,
 *     while loops left at 8 silently skip the new subsystem.
 *   - The shipped, gated feature injects into the *existing* slot 7 and never
 *     changes COUNT, so nothing today consumes the rewritten immediate.
 *
 * Runtime detection of the gap (how a future Action catches a missed patch,
 * without trusting this pass):
 *   - Reference half (done below): after patching, a second sweep re-reads
 *     every pair and counts any that still resolves into the old-array
 *     window.  It logs `residual=<n>` and pr_warn()s on a non-zero count, so a
 *     missed/again-broken reference is surfaced, never silent.
 *   - Count half (device only; needs a real 9th subsystem installed): the
 *     kernel's own controller list (/proc/cgroups) is produced by iterating
 *     cgroup_subsys[].  Compare its controller lines against the non-NULL,
 *     enabled entries of cgroup_subsys[]: an entry present in the array but
 *     absent from /proc/cgroups means a folded CGROUP_SUBSYS_COUNT bound was
 *     not widened -- the exact symptom of this gap.
 */
static bool dnsk_rw_reloc;
module_param_named(rw_reloc, dnsk_rw_reloc, bool, 0444);

static int dnsk_rwprobe_reloc(void)
{
	struct cgroup_subsys **old = dnsk_subsys_array;
	struct cgroup_subsys **neo;
	void *stext = dnsk_native_api->resolve("_stext");
	void *etext = dnsk_native_api->resolve("_etext");
	u64 olda = (u64)old;
	int i, patched = 0, residual = 0;
	u32 *p, i0, i1, buf[2];
	u64 tgt;
	enum dnsk_ref_kind kind;
	enum dnsk_reloc_result res;

	neo = kzalloc((DNSK_CGROUP_SUBSYS_COUNT + 1) * sizeof(void *), GFP_KERNEL);
	if (!neo)
		return -ENOMEM;
	for (i = 0; i < DNSK_CGROUP_SUBSYS_COUNT; i++)
		neo[i] = old[i];
	if (!stext || !etext || etext <= stext) {
		kfree(neo);
		return -ENOENT;
	}
	for (p = stext; (void *)(p + 2) <= etext; p++) {
		i0 = READ_ONCE(p[0]);
		i1 = READ_ONCE(p[1]);
		res = dnsk_reloc_pair((u64)p, i0, i1, olda, (u64)neo, buf);
		if (res != DNSK_RELOC_PATCH)
			continue;
		{
			struct dnsk_rw_req req = {0};

			req.opcode = DNSK_RW_WRITE;
			req.addr = (void *)p;
			req.src = buf;
			req.len = sizeof(buf);
			req.flags = DNSK_RW_F_TEXT;
			if (!dnsk_native_api->rw(&req))
				patched++;
		}
	}
	/*
	 * Verification sweep: count every pair that STILL resolves into the old
	 * array window.  A non-zero residual means a reference the reloc failed
	 * to retarget (unhandled encoding, or a failed F_TEXT write).  This is
	 * the runtime detection for the *reference* half of the gap; it does not
	 * cover CGROUP_SUBSYS_COUNT immediates (see the comment above).
	 */
	for (p = stext; (void *)(p + 2) <= etext; p++) {
		i0 = READ_ONCE(p[0]);
		i1 = READ_ONCE(p[1]);
		kind = dnsk_ref_pair((u64)p, i0, i1, &tgt);
		if (kind != DNSK_REF_NONE && dnsk_ref_in_window(tgt, olda))
			residual++;
	}
	pr_info("dnsk_rwprobe: reloc %px->%px patched=%d residual=%d\n", old, neo, patched,
		residual);
	if (residual)
		pr_warn("dnsk_rwprobe: %d reference(s) still target %px, reloc incomplete\n",
			residual, old);
	if (!patched) {
		kfree(neo);
		return -ENOENT;
	}
	dnsk_subsys_array = neo;
	return 0;
}

static void dnsk_rwprobe_run(void)
{
	struct dnsk_rw_req req = {0};
	struct cgroup_subsys *ss;
	void *namep;
	char name[40];
	void *arr = NULL;
	int i, ret;

	dnsk_rwprobe_probe("cgroup_subsys");
	dnsk_rwprobe_probe("cgroup_mutex");
	dnsk_rwprobe_probe("cgroup_subsys_count");
	dnsk_rwprobe_probe("_stext");
	dnsk_rwprobe_probe("_etext");

	req.opcode = DNSK_RW_PROBE;
	req.src = "cgroup_subsys";
	req.dst = &arr;
	req.ret = dnsk_native_api->rw(&req);
	if (req.ret) {
		pr_info("dnsk_rwprobe: no cgroup_subsys, ret=%d\n", req.ret);
		return;
	}
	for (i = 0; i <= DNSK_CGROUP_SUBSYS_COUNT; i++) {
		ss = NULL;
		req.opcode = DNSK_RW_READ;
		req.addr = (char *)arr + i * sizeof(void *);
		req.dst = &ss;
		req.len = sizeof(ss);
		ret = dnsk_native_api->rw(&req);
		if (ret) {
			pr_info("dnsk_rwprobe: slot %d read failed %d\n", i, ret);
			continue;
		}
		if (!ss) {
			pr_info("dnsk_rwprobe: slot %d NULL\n", i);
			continue;
		}
		name[0] = '\0';
		namep = NULL;
		req.opcode = DNSK_RW_READ;
		req.addr = (char *)ss + DNSK_CGROUP_SUBSYS_NAME;
		req.dst = &namep;
		req.len = sizeof(namep);
		if (!dnsk_native_api->rw(&req) && namep) {
			req.opcode = DNSK_RW_READ;
			req.addr = namep;
			req.dst = name;
			req.len = sizeof(name) - 1;
			dnsk_native_api->rw(&req);
		}
		if (!name[0])
			pr_info("dnsk_rwprobe: slot %d ss=%px name unreadable\n", i, ss);
		else
			pr_info("dnsk_rwprobe: slot %d ss=%px name=%s\n", i, ss, name);
	}
	dnsk_rwprobe_scan_refs(arr);
}

#define DNSK_SS_PTR(ss, off) (*(void **)((char *)(ss) + (off)))
#define DNSK_SS_SET(off, v) (*(void **)(dnsk_ss + (off)) = (void *)(v))
#define DNSK_SS_INT(off, v) (*(int *)(dnsk_ss + (off)) = (int)(v))
#define DNSK_PIDS(ptr) container_of(ptr, struct dnsk_pids_css, css)
#define DNSK_PARENT(p) container_of((p)->css.parent, struct dnsk_pids_css, css)

static ssize_t dnsk_pids_max_write(struct kernfs_open_file *of, char *buf, size_t nbytes,
				   loff_t off);
static int dnsk_pids_max_show(struct seq_file *sf, void *v);
static s64 dnsk_pids_current_read(struct cgroup_subsys_state *css, struct cftype *cft);
static s64 dnsk_pids_peak_read(struct cgroup_subsys_state *css, struct cftype *cft);
static int dnsk_pids_events_show(struct seq_file *sf, void *v);
static int dnsk_pids_events_local_show(struct seq_file *seq, void *v);

static struct cftype dnsk_native_files[] = {
    {
	.name = "max",
	.write = dnsk_pids_max_write,
	.seq_show = dnsk_pids_max_show,
	.flags = CFTYPE_NOT_ON_ROOT,
    },
    {
	.name = "current",
	.read_s64 = dnsk_pids_current_read,
	.flags = CFTYPE_NOT_ON_ROOT,
    },
    {
	.name = "peak",
	.read_s64 = dnsk_pids_peak_read,
	.flags = CFTYPE_NOT_ON_ROOT,
    },
    {
	.name = "events",
	.seq_show = dnsk_pids_events_show,
	.file_offset = offsetof(struct dnsk_pids_css, events_file),
	.flags = CFTYPE_NOT_ON_ROOT,
    },
    {
	.name = "events.local",
	.seq_show = dnsk_pids_events_local_show,
	.file_offset = offsetof(struct dnsk_pids_css, events_local_file),
	.flags = CFTYPE_NOT_ON_ROOT,
    },
    {}};

static __nocfi noinline struct cgroup_subsys_state *dnsk_call_of_css(struct kernfs_open_file *of)
{
	return dnsk_native_of_css(of);
}

static struct cgroup_subsys_state *dnsk_seq_css(struct seq_file *seq)
{
	struct kernfs_open_file *of = *(void **)((char *)seq + DNSK_SEQ_FILE_PRIVATE);

	return dnsk_native_of_css ? dnsk_call_of_css(of) : NULL;
}

static __nocfi noinline void dnsk_call_file_notify(struct cgroup_file *cfile)
{
	dnsk_cgroup_file_notify(cfile);
}

static void dnsk_pids_update_watermark(struct dnsk_pids_css *p, s64 nr)
{
	if (nr > READ_ONCE(p->watermark))
		WRITE_ONCE(p->watermark, nr);
}

static void dnsk_native_cancel(struct dnsk_pids_css *p, int num)
{
	WARN_ON_ONCE(atomic64_add_negative(-num, &p->counter));
}

static void dnsk_native_uncharge(struct dnsk_pids_css *p, int num)
{
	struct dnsk_pids_css *pos;

	for (pos = p; pos->css.parent; pos = DNSK_PARENT(pos))
		dnsk_native_cancel(pos, num);
}

static void dnsk_native_charge(struct dnsk_pids_css *p, int num)
{
	struct dnsk_pids_css *pos;

	for (pos = p; pos->css.parent; pos = DNSK_PARENT(pos)) {
		s64 new = atomic64_add_return(num, &pos->counter);

		dnsk_pids_update_watermark(pos, new);
	}
}

static int dnsk_native_try_charge(struct dnsk_pids_css *p, int num, struct dnsk_pids_css **fail)
{
	struct dnsk_pids_css *pos, *q;

	for (pos = p; pos->css.parent; pos = DNSK_PARENT(pos)) {
		s64 new = atomic64_add_return(num, &pos->counter);
		s64 limit = atomic64_read(&pos->limit);

		if (new > limit) {
			if (fail)
				*fail = pos;
			goto revert;
		}
		dnsk_pids_update_watermark(pos, new);
	}
	return 0;

revert:
	for (q = p; q != pos; q = DNSK_PARENT(q))
		dnsk_native_cancel(q, num);
	dnsk_native_cancel(pos, num);
	return -EAGAIN;
}

static void dnsk_pids_event(struct dnsk_pids_css *forking, struct dnsk_pids_css *over)
{
	struct dnsk_pids_css *p = forking;

	if (atomic64_inc_return(&p->events_local[DNSK_PIDCG_FORKFAIL]) == 1)
		pr_info("cgroup: fork rejected by pids controller\n");
	atomic64_inc(&over->events_local[DNSK_PIDCG_MAX]);
	if (dnsk_cgroup_file_notify)
		dnsk_call_file_notify(&over->events_local_file);

	for (p = over; p->css.parent; p = DNSK_PARENT(p)) {
		atomic64_inc(&p->events[DNSK_PIDCG_MAX]);
		if (dnsk_cgroup_file_notify)
			dnsk_call_file_notify(&p->events_file);
	}
}

static int dnsk_pids_can_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *dst_css;

	cgroup_taskset_for_each(task, dst_css, tset)
	{
		struct dnsk_pids_css *dst = DNSK_PIDS(dst_css);
		struct cgroup_subsys_state *old_css = task_css(task, DNSK_CGROUP_SUBSYS_SLOT);
		struct dnsk_pids_css *old;

		if (!old_css)
			continue;
		old = DNSK_PIDS(old_css);
		dnsk_native_charge(dst, 1);
		dnsk_native_uncharge(old, 1);
	}
	return 0;
}

static void dnsk_pids_cancel_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *dst_css;

	cgroup_taskset_for_each(task, dst_css, tset)
	{
		struct dnsk_pids_css *dst = DNSK_PIDS(dst_css);
		struct cgroup_subsys_state *old_css = task_css(task, DNSK_CGROUP_SUBSYS_SLOT);
		struct dnsk_pids_css *old;

		if (!old_css)
			continue;
		old = DNSK_PIDS(old_css);
		dnsk_native_charge(old, 1);
		dnsk_native_uncharge(dst, 1);
	}
}

static int dnsk_pids_can_fork(struct task_struct *task, struct css_set *cset)
{
	struct dnsk_pids_css *pids, *over = NULL;
	int ret;

	if (!cset || !cset->subsys[DNSK_CGROUP_SUBSYS_SLOT])
		return -EAGAIN;
	pids = DNSK_PIDS(cset->subsys[DNSK_CGROUP_SUBSYS_SLOT]);
	ret = dnsk_native_try_charge(pids, 1, &over);
	if (ret)
		dnsk_pids_event(pids, over);
	return ret;
}

static void dnsk_pids_cancel_fork(struct task_struct *task, struct css_set *cset)
{
	if (cset && cset->subsys[DNSK_CGROUP_SUBSYS_SLOT])
		dnsk_native_uncharge(DNSK_PIDS(cset->subsys[DNSK_CGROUP_SUBSYS_SLOT]), 1);
}

static void dnsk_pids_release(struct task_struct *task)
{
	struct cgroup_subsys_state *css = task_css(task, DNSK_CGROUP_SUBSYS_SLOT);

	if (css)
		dnsk_native_uncharge(DNSK_PIDS(css), 1);
}

static struct cgroup_subsys_state *dnsk_pids_alloc(struct cgroup_subsys_state *parent)
{
	struct dnsk_pids_css *pids = kzalloc(sizeof(*pids), GFP_KERNEL);

	if (!pids)
		return NULL;
	atomic64_set(&pids->limit, DNSK_PIDS_MAX);
	return &pids->css;
}

static void dnsk_pids_free(struct cgroup_subsys_state *css)
{
	kfree(DNSK_PIDS(css));
}

static ssize_t dnsk_pids_max_write(struct kernfs_open_file *of, char *buf, size_t nbytes,
				   loff_t off)
{
	struct cgroup_subsys_state *css = dnsk_call_of_css(of);
	struct dnsk_pids_css *pids;
	s64 limit;
	int err;

	if (!css)
		return -ENODEV;
	pids = DNSK_PIDS(css);
	buf = strstrip(buf);
	if (!strcmp(buf, DNSK_PIDS_MAX_STR)) {
		limit = DNSK_PIDS_MAX;
		goto set_limit;
	}
	err = kstrtoll(buf, 0, &limit);
	if (err)
		return err;
	if (limit < 0 || limit >= DNSK_PIDS_MAX)
		return -EINVAL;
set_limit:
	atomic64_set(&pids->limit, limit);
	return nbytes;
}

static int dnsk_pids_max_show(struct seq_file *sf, void *v)
{
	struct cgroup_subsys_state *css = dnsk_seq_css(sf);
	struct dnsk_pids_css *pids;
	s64 limit;

	if (!css)
		return -ENODEV;
	pids = DNSK_PIDS(css);
	limit = atomic64_read(&pids->limit);
	if (limit >= DNSK_PIDS_MAX)
		seq_puts(sf, DNSK_PIDS_MAX_STR "\n");
	else
		seq_printf(sf, "%lld\n", limit);
	return 0;
}

static s64 dnsk_pids_current_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return atomic64_read(&DNSK_PIDS(css)->counter);
}

static s64 dnsk_pids_peak_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return READ_ONCE(DNSK_PIDS(css)->watermark);
}

static int dnsk_pids_events_show(struct seq_file *sf, void *v)
{
	struct cgroup_subsys_state *css = dnsk_seq_css(sf);

	if (!css)
		return -ENODEV;
	seq_printf(sf, "max %lld\n", (s64)atomic64_read(&DNSK_PIDS(css)->events[DNSK_PIDCG_MAX]));
	return 0;
}

static int dnsk_pids_events_local_show(struct seq_file *seq, void *v)
{
	struct cgroup_subsys_state *css = dnsk_seq_css(seq);

	if (!css)
		return -ENODEV;
	seq_printf(seq, "max %lld\n",
		   (s64)atomic64_read(&DNSK_PIDS(css)->events_local[DNSK_PIDCG_MAX]));
	return 0;
}

static void dnsk_cset_set_slot(struct cgroup_subsys_state *css)
{
	int i;

	spin_lock(dnsk_css_set_lock);
	for (i = 0; i < DNSK_CSS_SET_BUCKETS; i++) {
		struct css_set *cset;

		hlist_for_each_entry(cset, &dnsk_css_set_table[i], hlist)
		    WRITE_ONCE(cset->subsys[DNSK_CGROUP_SUBSYS_SLOT], css);
	}
	spin_unlock(dnsk_css_set_lock);
}

static int dnsk_native_resolve(struct dnsk_core_api *api)
{
	void *key_array;

	dnsk_native_api = api;
	dnsk_subsys_array = api->resolve("cgroup_subsys");
	dnsk_root = api->resolve("cgrp_dfl_root");
	dnsk_init_cset = api->resolve("init_css_set");
	dnsk_have_canfork = api->resolve("have_canfork_callback");
	dnsk_have_release = api->resolve("have_release_callback");
	dnsk_css_set_lock = api->resolve("css_set_lock");
	dnsk_css_set_table = api->resolve("css_set_table");
	dnsk_cgroup_mutex = api->resolve("cgroup_mutex");
	dnsk_native_of_css = api->resolve("of_css");
	dnsk_add_dfl_cftypes = api->resolve("cgroup_add_dfl_cftypes");
	dnsk_inhibit_mask = api->resolve("cgrp_dfl_inhibit_ss_mask");
	dnsk_static_key_enable = api->resolve("static_key_enable");
	dnsk_static_key_disable = api->resolve("static_key_disable");
	dnsk_cgroup_file_notify = api->resolve("cgroup_file_notify");
	key_array = api->resolve("cgroup_subsys_on_dfl_key");
	if (key_array)
		dnsk_on_dfl_keys = key_array;
	if (dnsk_on_dfl_keys)
		dnsk_on_dfl_key = (struct static_key *)dnsk_on_dfl_keys[DNSK_CGROUP_SUBSYS_SLOT];

	if (!dnsk_subsys_array)
		pr_err("dnsk_cgpids: missing cgroup_subsys\n");
	if (!dnsk_root)
		pr_err("dnsk_cgpids: missing cgrp_dfl_root\n");
	if (!dnsk_init_cset)
		pr_err("dnsk_cgpids: missing init_css_set\n");
	if (!dnsk_css_set_lock)
		pr_err("dnsk_cgpids: missing css_set_lock\n");
	if (!dnsk_css_set_table)
		pr_err("dnsk_cgpids: missing css_set_table\n");
	if (!dnsk_cgroup_mutex)
		pr_err("dnsk_cgpids: missing cgroup_mutex\n");
	if (!dnsk_native_of_css)
		pr_err("dnsk_cgpids: missing of_css\n");
	if (!dnsk_add_dfl_cftypes)
		pr_err("dnsk_cgpids: missing cgroup_add_dfl_cftypes\n");
	if (!dnsk_inhibit_mask)
		pr_err("dnsk_cgpids: missing cgrp_dfl_inhibit_ss_mask\n");
	if (!dnsk_static_key_enable)
		pr_err("dnsk_cgpids: missing static_key_enable\n");
	if (!dnsk_on_dfl_key)
		pr_err("dnsk_cgpids: missing cgroup_subsys_on_dfl_key slot\n");
	if (!dnsk_subsys_array || !dnsk_root || !dnsk_init_cset || !dnsk_have_canfork ||
	    !dnsk_have_release || !dnsk_css_set_lock || !dnsk_css_set_table || !dnsk_cgroup_mutex ||
	    !dnsk_native_of_css || !dnsk_add_dfl_cftypes || !dnsk_inhibit_mask ||
	    !dnsk_static_key_enable || !dnsk_on_dfl_key)
		return -ENOENT;
	return 0;
}

static void dnsk_build_ss(void)
{
	memset(dnsk_ss, 0, sizeof(dnsk_ss));
	DNSK_SS_SET(DNSK_SS_CSS_ALLOC, dnsk_pids_alloc);
	DNSK_SS_SET(DNSK_SS_CSS_FREE, dnsk_pids_free);
	DNSK_SS_SET(DNSK_SS_CAN_ATTACH, dnsk_pids_can_attach);
	DNSK_SS_SET(DNSK_SS_CANCEL_ATTACH, dnsk_pids_cancel_attach);
	DNSK_SS_SET(DNSK_SS_CAN_FORK, dnsk_pids_can_fork);
	DNSK_SS_SET(DNSK_SS_CANCEL_FORK, dnsk_pids_cancel_fork);
	DNSK_SS_SET(DNSK_SS_RELEASE, dnsk_pids_release);
	DNSK_SS_INT(DNSK_SS_ID, DNSK_CGROUP_SUBSYS_SLOT);
	DNSK_SS_SET(DNSK_SS_NAME, "pids");
	DNSK_SS_SET(DNSK_SS_LEGACY_NAME, "pids");
	DNSK_SS_SET(DNSK_SS_DFL_CFTYPES, dnsk_native_files);
	DNSK_SS_SET(DNSK_SS_LEGACY_CFTYPES, dnsk_native_files);
	DNSK_SS_SET(DNSK_SS_ROOT, dnsk_root);
	idr_init((struct idr *)(dnsk_ss + DNSK_SS_CSS_IDR));
	INIT_LIST_HEAD((struct list_head *)(dnsk_ss + DNSK_SS_CFTS));
}

/* cgroup_add_dfl_cftypes is not exported and has no KCFI preamble. */
static __nocfi noinline int dnsk_call_add_dfl(struct cgroup_subsys *ss, struct cftype *cfts)
{
	return dnsk_add_dfl_cftypes(ss, cfts);
}

static __nocfi noinline void dnsk_call_skey_enable(struct static_key *key)
{
	dnsk_static_key_enable(key);
}

static __nocfi noinline void dnsk_call_skey_disable(struct static_key *key)
{
	if (dnsk_static_key_disable)
		dnsk_static_key_disable(key);
}

static struct cgroup_subsys_state *dnsk_make_root_css(void)
{
	struct cgroup_subsys_state *css;

	css = dnsk_pids_alloc(NULL);
	if (!css)
		return ERR_PTR(-ENOMEM);
	css->cgroup = &dnsk_root->cgrp;
	css->ss = (struct cgroup_subsys *)dnsk_ss;
	css->id = 1;
	css->flags = CSS_NO_REF | CSS_ONLINE;
	css->parent = NULL;
	INIT_LIST_HEAD(&css->sibling);
	INIT_LIST_HEAD(&css->children);
	INIT_LIST_HEAD(&css->rstat_css_node);
	atomic_set(&css->online_cnt, 1);

	rcu_assign_pointer(dnsk_root->cgrp.subsys[DNSK_CGROUP_SUBSYS_SLOT], css);
	rcu_assign_pointer(dnsk_init_cset->subsys[DNSK_CGROUP_SUBSYS_SLOT], css);
	dnsk_cset_set_slot(css);
	return css;
}

/*
 * Hand the slot back everywhere it was written.  dnsk_cset_set_slot() replaced
 * the pointer in *every* css_set, so restoring only the root and init_css_set
 * would leave all other sets pointing at a cs object that is about to be freed
 * together with this module -- a dangling pointer reachable from any task.
 */
static void dnsk_restore_root_css(struct cgroup_subsys_state *old)
{
	rcu_assign_pointer(dnsk_root->cgrp.subsys[DNSK_CGROUP_SUBSYS_SLOT], old);
	rcu_assign_pointer(dnsk_init_cset->subsys[DNSK_CGROUP_SUBSYS_SLOT], old);
	dnsk_cset_set_slot(old);
}

int dnsk_cgpids_native_init(struct dnsk_core_api *api)
{
	struct cgroup_subsys *old_ss;
	struct cgroup_subsys_state *old_root_css;
	struct cgroup_subsys_state *css;
	u16 old_inhibit;
	bool key_was_enabled;
	int ret;

	pr_info("dnsk_cgpids: native init entry\n");
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	ret = dnsk_native_resolve(api);
	if (ret)
		goto out_put;

	if (dnsk_rw_probe)
		dnsk_rwprobe_run();

	if (dnsk_rw_reloc) {
		ret = dnsk_rwprobe_reloc();
		if (ret)
			goto out_put;
	}

	old_ss = dnsk_subsys_array[DNSK_CGROUP_SUBSYS_SLOT];
	if (!old_ss) {
		ret = -ENOENT;
		goto out_put;
	}
	if (strcmp(DNSK_SS_PTR(old_ss, DNSK_CGROUP_SUBSYS_NAME), "net_prio")) {
		pr_err("dnsk_cgpids: slot %d is %s, not net_prio\n", DNSK_CGROUP_SUBSYS_SLOT,
		       (char *)DNSK_SS_PTR(old_ss, DNSK_CGROUP_SUBSYS_NAME));
		ret = -EPROTO;
		goto out_put;
	}
	old_root_css =
	    rcu_dereference_protected(dnsk_root->cgrp.subsys[DNSK_CGROUP_SUBSYS_SLOT], 1);
	if (!old_root_css) {
		ret = -ENOENT;
		goto out_put;
	}

	dnsk_build_ss();
	ret = dnsk_call_add_dfl((struct cgroup_subsys *)dnsk_ss, dnsk_native_files);
	if (ret)
		goto out_put;

	mutex_lock(dnsk_cgroup_mutex);
	old_inhibit = READ_ONCE(*dnsk_inhibit_mask);
	WRITE_ONCE(*dnsk_inhibit_mask, old_inhibit & ~BIT(DNSK_CGROUP_SUBSYS_SLOT));
	key_was_enabled = static_key_enabled(dnsk_on_dfl_key);
	dnsk_call_skey_enable(dnsk_on_dfl_key);

	css = dnsk_make_root_css();
	if (IS_ERR(css)) {
		ret = PTR_ERR(css);
		goto out_disable_key;
	}

	ret = dnsk_native_api->patch_pointer(&dnsk_subsys_array[DNSK_CGROUP_SUBSYS_SLOT], dnsk_ss);
	if (ret) {
		dnsk_restore_root_css(old_root_css);
		goto out_disable_key;
	}

	WRITE_ONCE(*dnsk_have_canfork, *dnsk_have_canfork | BIT(DNSK_CGROUP_SUBSYS_SLOT));
	WRITE_ONCE(*dnsk_have_release, *dnsk_have_release | BIT(DNSK_CGROUP_SUBSYS_SLOT));
	mutex_unlock(dnsk_cgroup_mutex);

	dnsk_saved_ss = old_ss;
	dnsk_saved_root_css = old_root_css;
	dnsk_own_root_css = css;
	dnsk_saved_inhibit = old_inhibit;
	dnsk_saved_key_was_enabled = key_was_enabled;
	dnsk_saved_have_canfork = *dnsk_have_canfork & ~BIT(DNSK_CGROUP_SUBSYS_SLOT);
	dnsk_saved_have_release = *dnsk_have_release & ~BIT(DNSK_CGROUP_SUBSYS_SLOT);
	dnsk_native_ready = true;
	/*
	 * This kernel's cgroup_subsys has no `owner` field (verified against the
	 * device BTF: 32 members, ending at depends_on), so cgroup core cannot hold
	 * a reference on our behalf.  The reference taken above is therefore the
	 * only thing keeping this module resident -- and it is exactly what stops
	 * rmmod from ever reaching module_exit, a self-inflicted deadlock.  It is
	 * dropped by dnsk_cgpids_native_quiesce() after the slot has been handed
	 * back, so unloading becomes possible while nothing runs our code.
	 */
	dnsk_self_ref = true;
	pr_info("dnsk_cgpids: native pids registered in slot %d\n", DNSK_CGROUP_SUBSYS_SLOT);
	return 0;

out_disable_key:
	if (!key_was_enabled)
		dnsk_call_skey_disable(dnsk_on_dfl_key);
	WRITE_ONCE(*dnsk_inhibit_mask, old_inhibit);
	mutex_unlock(dnsk_cgroup_mutex);
out_put:
	module_put(THIS_MODULE);
	return ret;
}

int dnsk_cgpids_native_exit(void)
{
	s64 live;
	int ret;

	if (!dnsk_native_ready)
		return 0;

	/* The root css counts every task still attached to the emulated
	 * controller.  Read it from the object we installed, never from the slot:
	 * the slot can already have been handed back to the previous subsystem. */
	live = dnsk_own_root_css ? atomic64_read(&DNSK_PIDS(dnsk_own_root_css)->counter) : 0;
	/*
	 * Refuse to hand the slot back while tasks are still accounted to it.
	 * cgroup core would keep calling our css_free/release callbacks after this
	 * returns, and once the module is unloaded those entry points are gone --
	 * that is precisely the dangling-pointer case this whole exercise is about.
	 * The caller must drain its cgroups first; the error is reported, not
	 * papered over.
	 */
	if (live) {
		pr_err("dnsk_cgpids: refusing to release slot %d with %lld live pids\n",
		       DNSK_CGROUP_SUBSYS_SLOT, live);
		return -EBUSY;
	}

	mutex_lock(dnsk_cgroup_mutex);
	WRITE_ONCE(*dnsk_have_canfork, dnsk_saved_have_canfork);
	WRITE_ONCE(*dnsk_have_release, dnsk_saved_have_release);
	ret = dnsk_native_api->patch_pointer(&dnsk_subsys_array[DNSK_CGROUP_SUBSYS_SLOT],
					     dnsk_saved_ss);
	if (ret)
		pr_err("dnsk_cgpids: slot restore failed: %d\n", ret);
	dnsk_restore_root_css(dnsk_saved_root_css);
	WRITE_ONCE(*dnsk_inhibit_mask, dnsk_saved_inhibit);
	if (!dnsk_saved_key_was_enabled)
		dnsk_call_skey_disable(dnsk_on_dfl_key);
	mutex_unlock(dnsk_cgroup_mutex);

	dnsk_own_root_css = NULL;
	dnsk_native_ready = false;
	/*
	 * Drop exactly the reference init took.  If quiesce() already ran, this is
	 * what finally lets rmmod proceed; if it did not, this is the reference that
	 * would otherwise keep the module resident forever.
	 */
	if (dnsk_self_ref) {
		dnsk_self_ref = false;
		module_put(THIS_MODULE);
	}
	pr_info("dnsk_cgpids: native pids released slot %d\n", DNSK_CGROUP_SUBSYS_SLOT);
	return 0;
}

/*
 * Hand the slot back and drop the self reference, without unloading.
 *
 * rmmod cannot run module_exit while the module holds a reference to itself, so
 * teardown happens in two steps: this call restores the slot, switches the
 * static key off and swaps the root css back, and only then does the module
 * reference drop, letting a subsequent rmmod reach module_exit.  Idempotent.
 */
int dnsk_cgpids_native_quiesce(void)
{
	if (!dnsk_native_ready)
		return 0;
	/*
	 * The module reference taken by init must survive until the slot is really
	 * ours no longer: dropping it first would let rmmod free this text while
	 * cgroup core could still call into it.
	 */
	return dnsk_cgpids_native_exit();
}

bool dnsk_cgpids_native_active(void)
{
	return dnsk_native_ready;
}

#endif /* DNSK_CGROUP_SUBSYS_SLOT >= 0 */
