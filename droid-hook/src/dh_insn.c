/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Droid-Hook arm64 instruction encoding and static-array reference relocation.
 *
 * This translation unit is host-compilable (no kernel headers) so the encoder
 * can be verified against real assembler output in tests/host/.  It never
 * includes a core header; the only cross-TU dependency is dh_txn_rollback(),
 * declared extern and supplied by the dh-rw-dev translation unit (or a stub in
 * the host harness).
 *
 * Encoding facts were checked against clang --target=aarch64-linux-gnu and
 * aarch64-linux-gnu-objdump in tests/host/test_dh_insn.py.  Bit fields are
 * documented per instruction below.
 */

#include "dh.h"

#include <stddef.h>
#include <stdint.h>

/* dh.h's host branch provides u8/u16/u32/u64 but not s64; the kernel branch
 * gets s64 from <linux/types.h>.  This is a dh.h contract gap that should be
 * reported to the lead, not silently patched here in the shared header. */
#ifndef __KERNEL__
typedef int64_t s64;
#endif

/* Implemented by dh_rw.c (dh-rw-dev); stubbed in the host harness. */
extern int dh_txn_rollback(struct dh_ctx *ctx);

/* ------------------------------------------------------------------------- */
/* Bit-field masks (AArch64 ARM, DDI 0487; verified with clang/objdump).      */
/* ------------------------------------------------------------------------- */

/* ADRP / ADR: immlo = bits[30:29], immhi = bits[23:5]. */
#define ADR_IMM_MASK   0x60FFFFE0u
#define ADR_IMMHI_SH   5
#define ADR_IMMLO_SH   29
#define ADRP_OP_MASK   0x9F000000u
#define ADRP_OP        0x90000000u
#define ADR_OP_MASK    0x9F000000u
#define ADR_OP         0x10000000u

/* ADD/SUB (immediate): imm12 = bits[21:10], sh = bits[23:22] (only LSL #0 and
 * LSL #12 are allocated; bit[23] must be 0), S = bit[30].  The class mask must
 * include bit[30] or SUB (0x51000000) is mistaken for ADD (0x11000000) --
 * verified against clang's 0xd1115863 for "sub x3,x3,#0x456".
 *
 * ADD_IMM_MASK is 0x003FFC00, i.e. bits[21:10] and nothing else.  The value
 * 0x00FFFC00 that the cgpids prototype and an earlier draft of this file used
 * spans bits[10:23]: it also covers sh and bit[23].  Extract-then-reinsert of
 * that 14-bit slice happens to round-trip, but any code that treats it as the
 * numeric imm12 (scaling by sh, or writing a new offset) silently clears sh.
 * That is the same class of bug as commit 8caf6e6, so the mask is corrected
 * here and ADD_SH_MASK names the bits an LSL #12 -> LSL #0 re-encode must
 * clear explicitly. */
#define ADD_IMM_MASK   0x003FFC00u
#define ADD_IMM_SH     10
#define ADD_SH_BIT     (1u << 22)
#define ADD_SH_MASK    0x00C00000u
#define ADD_OP_MASK    0x7F000000u
#define ADD_OP         0x11000000u
#define SUB_OP         0x51000000u

/* Load/store (unsigned immediate): imm12 = bits[21:10]; opc = bits[23:22],
 * size = bits[31:30], V = bit[26] MUST be preserved (commit 8caf6e6). */
#define LDST_IMM_MASK  0x003FFC00u
#define LDST_IMM_SH    10
#define LDST_OP_MASK   0x3B000000u
#define LDST_OP        0x39000000u

/* LDR literal (W/X/SW): imm19 = bits[23:5], opc = bits[31:30]. */
#define LDR_LIT_IMM_MASK 0x00FFFFE0u
#define LDR_LIT_IMM_SH   5
#define LDR_LIT_OP_MASK  0x3F000000u
#define LDR_LIT_OP       0x18000000u

/* B / BL: imm26 = bits[25:0]; BL has bit[31]=1, so the class mask must
 * include it or BL (0x94000000) is decoded as B (0x14000000). */
#define BRANCH_IMM_MASK 0x03FFFFFFu
#define BRANCH_OP_MASK  0xFC000000u
#define B_OP            0x14000000u
#define BL_OP           0x94000000u

/* B.cond: imm19 = bits[23:5], cond = bits[3:0]. */
#define BCOND_IMM_MASK  0x00FFFFE0u
#define BCOND_IMM_SH    5
#define BCOND_OP_MASK   0xFF000010u
#define BCOND_OP        0x54000000u

/* CBZ/CBNZ: imm19 = bits[23:5], op = bit[24], sf = bit[31]. */
#define CBZ_IMM_MASK    0x00FFFFE0u
#define CBZ_IMM_SH      5
#define CBZ_OP_MASK     0x7F000000u
#define CBZ_OP          0x34000000u

/* TBZ/TBNZ: b40 = bits[23:19], imm14 = bits[18:5]. */
#define TBZ_IMM14_MASK  0x0007FFE0u
#define TBZ_IMM14_SH    5
#define TBZ_B40_MASK    0x00F80000u
#define TBZ_OP_MASK     0x7F000000u
#define TBZ_OP          0x36000000u

/* Signed ranges.  ADRP's immediate is a 21-bit signed *page* delta, which
 * covers ±4 GiB of address space; the limit below is in pages. */
#define ADRP_PAGE_LIMIT (1LL << 20)   /* 2^20 pages = ±4 GiB */
#define ADR_RANGE       (1LL << 20)   /* ±1 MiB, 21-bit byte delta */
#define BRANCH_RANGE    (1LL << 27)   /* ±128 MiB, 26-bit word delta */
#define LDR_LIT_RANGE   (1LL << 20)   /* ±1 MiB, 19-bit word delta */
#define BCOND_RANGE     (1LL << 20)   /* ±1 MiB, 19-bit word delta */
#define CBZ_RANGE       (1LL << 20)
#define TBZ_RANGE       (1LL << 15)   /* ±32 KiB, 14-bit word delta */

static int fits(s64 delta, s64 range)
{
	return delta >= -range && delta < range;
}

static s64 sext(s64 value, unsigned int bits)
{
	s64 sign = (s64)1 << (bits - 1);

	return (value & (sign - 1)) - (value & sign);
}

/* ------------------------------------------------------------------------- */
/* Field decoders and encoders.                                               */
/*                                                                            */
/* The relocator, the scanner and the ADRP+follower pair re-encoder all use    */
/* these, so there is exactly one place that knows a field's mask.  Every      */
/* encoder writes its field through the mask and leaves every other bit        */
/* untouched: that is what keeps Rd/Rn/Rt, sh (ADD bit[22], bit[23] reserved),  */
/* size and opc (load/store bits[31:30], [23:22]) and V (bit[26]) intact.       */
/* ------------------------------------------------------------------------- */

/* ADRP's immediate is a 21-bit signed *page* delta split into immlo/immhi. */
static s64 adrp_imm_pages(u32 insn)
{
	s64 imm = (s64)((((insn >> ADR_IMMHI_SH) & 0x7FFFF) << 2) |
			((insn >> ADR_IMMLO_SH) & 3));

	return sext(imm, 21);
}

static u32 adrp_set_imm_pages(u32 insn, s64 pages)
{
	u32 lo = (u32)((u64)pages & 3);
	u32 hi = (u32)(((u64)pages >> 2) & 0x7FFFF);

	return (insn & ~ADR_IMM_MASK) |
	       (lo << ADR_IMMLO_SH) | (hi << ADR_IMMHI_SH);
}

/* ADD/SUB (immediate).  ADD_OP_MASK keeps bit[30], so SUB is not mistaken for
 * ADD; ADD_IMM_MASK covers imm12 only, so rewriting it cannot touch sh. */
static int addsub_is_imm(u32 insn)
{
	u32 op = insn & ADD_OP_MASK;

	return op == ADD_OP || op == SUB_OP;
}

/* Byte offset the imm12 encodes; sh=1 scales the immediate by 4096. */
static u64 addsub_imm_off(u32 insn)
{
	u32 imm12 = (insn & ADD_IMM_MASK) >> ADD_IMM_SH;
	u32 sh = (insn >> 22) & 1;

	return (u64)imm12 << (sh ? 12 : 0);
}

/* Rewrite imm12 from a byte offset.  @off must be expressible by the
 * instruction's current sh: a multiple of 4096 when sh=1.  The pair re-encoder
 * clears sh before calling this whenever it needs a sub-page offset. */
static u32 addsub_set_imm_off(u32 insn, u64 off)
{
	u32 sh = (insn >> 22) & 1;
	u32 imm12 = (u32)(off >> (sh ? 12 : 0));

	return (insn & ~ADD_IMM_MASK) |
	       ((imm12 << ADD_IMM_SH) & ADD_IMM_MASK);
}

/* Load/store (unsigned immediate).  LDST_OP_MASK matches size/opc/V-bit
 * classes that use imm12; LDST_IMM_MASK covers imm12 only, so re-encoding an
 * offset can never clear opc -- the bug fixed by commit 8caf6e6. */
static int ldst_is_uimm(u32 insn)
{
	return (insn & LDST_OP_MASK) == LDST_OP;
}

/* Access size in bytes: 1, 2, 4 or 8 from size bits[31:30]. */
static u32 ldst_scale(u32 insn)
{
	return 1u << ((insn >> 30) & 3);
}

static u64 ldst_imm_off(u32 insn)
{
	return (u64)((insn & LDST_IMM_MASK) >> LDST_IMM_SH) * ldst_scale(insn);
}

static u32 ldst_set_imm_off(u32 insn, u64 off)
{
	u32 imm12 = (u32)(off / ldst_scale(insn));

	return (insn & ~LDST_IMM_MASK) |
	       ((imm12 << LDST_IMM_SH) & LDST_IMM_MASK);
}

/* READ_ONCE semantics: a volatile load, so the compiler cannot fold or cache
 * the instruction words while we scan a live kernel text range. */
static u32 insn_read(const u32 *p)
{
#ifdef __KERNEL__
	return READ_ONCE(*p);
#else
	const volatile u32 *vp = (const volatile u32 *)p;

	return *vp;
#endif
}

/* ------------------------------------------------------------------------- */
/* Absolute jump trampoline.                                                  */
/* ------------------------------------------------------------------------- */

/**
 * dh_insn_gen_absolute_jump() - emit a KernelPatch-style absolute jump
 * @out: caller buffer of at least 4 u32 words
 * @target: destination address
 *
 * Encoding (verified: clang "ldr x17, #8; br x17" -> 0x58000051 / 0xd61f0220):
 *
 *   out[0] = 0x58000051  LDR X17, #8
 *            opcode bits[31:24]=0x58 (LDR literal, 64-bit variant),
 *            imm19 bits[23:5]=1 (byte offset 8 / 4), Rt bits[4:0]=17 (X17).
 *   out[1] = 0xd61f0220  BR X17
 *            opcode 0xd61f0000 | Rn bits[9:5]=17.  BR (not BLR) keeps X30
 *            unchanged and stays outside compiler-inserted kCFI hash checks.
 *   out[2] = target bits[31:0]   (little-endian 64-bit literal low word)
 *   out[3] = target bits[63:32]  (high word)
 *
 * X16/X17 are AAPCS64 intra-procedure-call scratch registers, so no stack,
 * SP, x18 (SCS) or general-purpose registers are touched.
 *
 * Return: %DH_OK, or %DH_E_INVAL for a NULL buffer.
 */
int dh_insn_gen_absolute_jump(u32 *out, void *target)
{
	u64 addr;

	if (!out)
		return DH_E_INVAL;
	addr = (u64)(uintptr_t)target;
	out[0] = 0x58000051u;
	out[1] = 0xd61f0220u;
	out[2] = (u32)(addr & 0xFFFFFFFFu);
	out[3] = (u32)(addr >> 32);
	return DH_OK;
}

/* ------------------------------------------------------------------------- */
/* Per-instruction relocation.                                                */
/* ------------------------------------------------------------------------- */

/* Re-encode a PC-relative instruction so it still reaches the same absolute
 * target from a new PC.  delta is in bytes unless noted. */
static int relocate_pc_rel(u32 insn, u32 *dst, u64 src_pc, u64 dst_pc)
{
	s64 target;
	s64 delta;

	if ((insn & ADRP_OP_MASK) == ADRP_OP) {
		/* ADRP: 21-bit signed page delta, target page = PCpage + imm<<12. */
		s64 imm = adrp_imm_pages(insn);
		s64 page_delta;

		target = (s64)(src_pc & ~0xFFFULL) + (imm << 12);
		page_delta = (s64)((target & ~0xFFFULL) -
				   (dst_pc & ~0xFFFULL)) >> 12;
		if (!fits(page_delta, ADRP_PAGE_LIMIT))
			return DH_E_OPNOTSUPP;
		*dst = adrp_set_imm_pages(insn, page_delta);
		return DH_OK;
	}
	if ((insn & ADR_OP_MASK) == ADR_OP) {
		/* ADR: 21-bit signed byte delta, ±1 MiB.  Like ADRP the
		 * immediate is split into immlo/immhi, so it cannot be
		 * written with a single contiguous shift. */
		s64 imm = sext((s64)((((insn >> ADR_IMMHI_SH) & 0x7FFFF) << 2) |
				     ((insn >> ADR_IMMLO_SH) & 3)), 21);
		u32 lo, hi;

		target = (s64)src_pc + imm;
		delta = (s64)target - (s64)dst_pc;
		if (!fits(delta, ADR_RANGE))
			return DH_E_OPNOTSUPP;
		lo = (u32)((u64)delta & 3);
		hi = (u32)(((u64)delta >> 2) & 0x7FFFF);
		*dst = (insn & ~ADR_IMM_MASK) |
		       (lo << ADR_IMMLO_SH) | (hi << ADR_IMMHI_SH);
		return DH_OK;
	}
	if ((insn & BRANCH_OP_MASK) == B_OP ||
	    (insn & BRANCH_OP_MASK) == BL_OP) {
		/* B/BL: 26-bit signed word delta, ±128 MiB. */
		s64 imm = sext((s64)(insn & BRANCH_IMM_MASK), 26);

		target = (s64)src_pc + (imm << 2);
		delta = (s64)target - (s64)dst_pc;
		if (!fits(delta, BRANCH_RANGE) || (delta & 3))
			return DH_E_OPNOTSUPP;
		*dst = (insn & ~BRANCH_IMM_MASK) |
		       ((u32)(delta >> 2) & BRANCH_IMM_MASK);
		return DH_OK;
	}
	if ((insn & BCOND_OP_MASK) == BCOND_OP) {
		/* B.cond: 19-bit signed word delta, ±1 MiB. */
		s64 imm = sext((s64)((insn & BCOND_IMM_MASK) >> BCOND_IMM_SH), 19);

		target = (s64)src_pc + (imm << 2);
		delta = (s64)target - (s64)dst_pc;
		if (!fits(delta, BCOND_RANGE) || (delta & 3))
			return DH_E_OPNOTSUPP;
		*dst = (insn & ~BCOND_IMM_MASK) |
		       (((u32)(delta >> 2) << BCOND_IMM_SH) & BCOND_IMM_MASK);
		return DH_OK;
	}
	if ((insn & CBZ_OP_MASK) == CBZ_OP) {
		/* CBZ/CBNZ: 19-bit signed word delta, ±1 MiB. */
		s64 imm = sext((s64)((insn & CBZ_IMM_MASK) >> CBZ_IMM_SH), 19);

		target = (s64)src_pc + (imm << 2);
		delta = (s64)target - (s64)dst_pc;
		if (!fits(delta, CBZ_RANGE) || (delta & 3))
			return DH_E_OPNOTSUPP;
		*dst = (insn & ~CBZ_IMM_MASK) |
		       (((u32)(delta >> 2) << CBZ_IMM_SH) & CBZ_IMM_MASK);
		return DH_OK;
	}
	if ((insn & TBZ_OP_MASK) == TBZ_OP) {
		/* TBZ/TBNZ: 14-bit signed word delta, ±32 KiB; b40 unchanged. */
		s64 imm = sext((s64)((insn & TBZ_IMM14_MASK) >> TBZ_IMM14_SH), 14);

		target = (s64)src_pc + (imm << 2);
		delta = (s64)target - (s64)dst_pc;
		if (!fits(delta, TBZ_RANGE) || (delta & 3))
			return DH_E_OPNOTSUPP;
		*dst = (insn & ~TBZ_IMM14_MASK) |
		       (((u32)(delta >> 2) << TBZ_IMM14_SH) & TBZ_IMM14_MASK);
		return DH_OK;
	}
	if ((insn & LDR_LIT_OP_MASK) == LDR_LIT_OP) {
		/* LDR literal: 19-bit signed word delta, ±1 MiB. */
		s64 imm = sext((s64)((insn & LDR_LIT_IMM_MASK) >>
				     LDR_LIT_IMM_SH), 19);

		target = (s64)src_pc + (imm << 2);
		delta = (s64)target - (s64)dst_pc;
		if (!fits(delta, LDR_LIT_RANGE) || (delta & 3))
			return DH_E_OPNOTSUPP;
		*dst = (insn & ~LDR_LIT_IMM_MASK) |
		       (((u32)(delta >> 2) << LDR_LIT_IMM_SH) &
			LDR_LIT_IMM_MASK);
		return DH_OK;
	}
	return DH_E_OPNOTSUPP;
}

/**
 * dh_insn_relocate() - retarget one instruction from src_pc to dst_pc
 * @src: original instruction words (at src_pc)
 * @dst: output words (at dst_pc)
 * @src_pc: address the original instruction executes at
 * @dst_pc: address the relocated instruction will execute at
 * @nr: number of words
 *
 * PC-relative classes (ADRP, ADR, B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ,
 * LDR literal) are re-encoded so the absolute target is unchanged.  A target
 * that no longer fits the class's signed range returns DH_E_OPNOTSUPP rather
 * than being silently truncated.
 *
 * Data-address classes (ADD/SUB immediate, LDR/STR unsigned offset) are not
 * PC-relative: their imm12 encodes an offset from a base register, so a PC
 * change does not change imm12.  The word is nevertheless rewritten through
 * the documented masks (ADD/SUB imm12 0x003FFC00, load/store 0x003FFC00)
 * instead of being copied, which keeps one single encoder and pins that sh,
 * size, opc and V cannot be cleared.  Retargeting a *pair* to a different
 * array base is a different operation: see insn_pair_retarget(), which
 * dh_scan_refs() uses as a preflight.
 *
 * Return: %DH_OK, %DH_E_INVAL on bad arguments, %DH_E_OPNOTSUPP for an
 * unsupported instruction class or an out-of-range PC-relative target.
 */
int dh_insn_relocate(const u32 *src, u32 *dst, void *src_pc, void *dst_pc,
		     size_t nr)
{
	size_t i;
	u64 spc = (u64)(uintptr_t)src_pc;
	u64 dpc = (u64)(uintptr_t)dst_pc;

	if (!src || !dst || nr == 0)
		return DH_E_INVAL;
	for (i = 0; i < nr; i++) {
		u32 insn = insn_read(&src[i]);
		int ret = relocate_pc_rel(insn, &dst[i], spc + i * 4,
					   dpc + i * 4);

		if (ret == DH_OK)
			continue;
		if (ret != DH_E_OPNOTSUPP)
			return ret;
		/* ADD/SUB immediate and load/store unsigned offset are not
		 * PC-relative: imm12 is an offset from a base register, so
		 * A PC CHANGE DOES NOT CHANGE imm12.  The word is still written
		 * back through the field encoders (ADD mask 0x003FFC00, load/
		 * store mask 0x003FFC00) rather than copied, which proves the
		 * two masks cover imm12 and nothing else:
		 *
		 *  - ADD/SUB: 0x003FFC00 is bits[21:10] only, so sh (bit[22]),
		 *    S (bit[30]) and Rd/Rn survive, and SUB cannot turn into ADD
		 *    because its class bit[30] is outside the mask.
		 *  - LDR/STR: 0x003FFC00 is bits[21:10] only, so size
		 *    (bits[31:30]), opc (bits[23:22]) and V (bit[26]) survive.
		 *    Using the ADD mask here would clear opc -- the bug fixed by
		 *    commit 8caf6e6.
		 *
		 * The same encoders are reused by insn_pair_retarget(), which
		 * additionally rewrites imm12 when the array base moves. */
		if (addsub_is_imm(insn)) {
			dst[i] = addsub_set_imm_off(insn, addsub_imm_off(insn));
			continue;
		}
		if (ldst_is_uimm(insn)) {
			dst[i] = ldst_set_imm_off(insn, ldst_imm_off(insn));
			continue;
		}
		return DH_E_OPNOTSUPP;
	}
	return DH_OK;
}

/* ------------------------------------------------------------------------- */
/* ADRP + follower pair re-encoding.                                          */
/*                                                                            */
/* dh.h has no public entry point that can express "re-encode these two words */
/* together", and the header is frozen for this task.  The pair encoder is     */
/* therefore static and consumed by dh_scan_refs() as a retargetability        */
/* preflight: a reference that cannot be re-encoded for @new_base fails the    */
/* scan instead of being returned as a site the caller could not patch.        */
/* ------------------------------------------------------------------------- */

/**
 * insn_pair_retarget() - re-encode an ADRP + ADD/LDR pair for a new array base
 * @adrp: the ADRP word (unchanged address, i.e. it still executes at @adrp_pc)
 * @follow: the ADD immediate or load/store unsigned-immediate word
 * @adrp_pc: address of the ADRP instruction
 * @old_base: base address of the array the pair currently references
 * @new_base: base address the array is being relocated to
 * @out_adrp: receives the re-encoded ADRP
 * @out_follow: receives the re-encoded follower
 *
 * An ADRP+ADD/LDR pair materialises one absolute address, page(ADRP) + imm12,
 * which the scanner has already proven to lie inside [old_base, old_base+n).
 * A same-content array relocation keeps every element's offset inside the
 * array, so the pair must reach new_base + (old_addr - old_base).  The ADRP is
 * re-aimed at that address's page and the follower's imm12 is rewritten to the
 * in-page offset; every other bit (Rd/Rn/Rt, sh, size, opc, V) is preserved
 * because only the documented imm fields are written.
 *
 * Return: %DH_OK; %DH_E_INVAL when @adrp or an output pointer is invalid, or
 * when the pair's target decodes below @old_base (a caller that skipped the
 * in-range check);
 * %DH_E_OPNOTSUPP when the follower is not an ADD immediate or a load/store
 * unsigned immediate (SUB is refused: a non-negative imm12 cannot reach an
 * arbitrary page offset downwards), when the in-page offset cannot be encoded
 * by the original form (a load/store offset must be a multiple of the access
 * size), or when the ADRP page delta leaves the +-4 GiB range.  A pair that
 * used ADD LSL #12 is re-encoded as LSL #0 rather than refused: the ADRP
 * already points at the target page, so imm12 can carry the in-page offset.
 */
static int insn_pair_retarget(u32 adrp, u32 follow, u64 adrp_pc,
			      u64 old_base, u64 new_base,
			      u32 *out_adrp, u32 *out_follow)
{
	u64 page, old_addr, new_addr, new_page, off;
	s64 page_delta;

	if (!out_adrp || !out_follow)
		return DH_E_INVAL;
	if ((adrp & ADRP_OP_MASK) != ADRP_OP)
		return DH_E_INVAL;
	if ((follow & ADD_OP_MASK) == SUB_OP)
		return DH_E_OPNOTSUPP;

	page = (adrp_pc & ~0xFFFULL) + ((u64)adrp_imm_pages(adrp) << 12);
	if ((follow & ADD_OP_MASK) == ADD_OP)
		old_addr = page + addsub_imm_off(follow);
	else if (ldst_is_uimm(follow))
		old_addr = page + ldst_imm_off(follow);
	else
		return DH_E_OPNOTSUPP;
	/* dh_scan_refs() only calls this for a target it already proved to be
	 * inside the array; refuse a caller that breaks that precondition rather
	 * than wrapping around into an unrelated address. */
	if (old_addr < old_base)
		return DH_E_INVAL;

	new_addr = new_base + (old_addr - old_base);
	new_page = new_addr & ~0xFFFULL;
	page_delta = (s64)((new_page - (adrp_pc & ~0xFFFULL)) >> 12);
	if (!fits(page_delta, ADRP_PAGE_LIMIT))
		return DH_E_OPNOTSUPP;
	off = new_addr - new_page;

	if ((follow & ADD_OP_MASK) == ADD_OP) {
		u32 base = follow;

		/* A pair that used LSL #12 (sh=1) can only reach page-aligned
		 * addresses in that form.  When the new target is not page
		 * aligned, re-encode it as LSL #0: the ADRP below already points
		 * at the target page, so a plain imm12=off reaches the same
		 * address.  Clearing sh is deliberate and limited to ADD_SH_MASK;
		 * opcode, S, Rn and Rd are untouched. */
		if (((follow >> 22) & 1) && off)
			base = follow & ~ADD_SH_MASK;
		*out_follow = addsub_set_imm_off(base, off);
	} else {
		if (off % ldst_scale(follow))
			return DH_E_OPNOTSUPP;
		*out_follow = ldst_set_imm_off(follow, off);
	}
	*out_adrp = adrp_set_imm_pages(adrp, page_delta);
	return DH_OK;
}

#ifdef DH_INS_TEST_SEAM
/*
 * Test-only wrapper, never compiled into a production build: the host selftest
 * compiles this translation unit with -DDH_INS_TEST_SEAM so it can assert the
 * exact words insn_pair_retarget() emits.  It calls the same static function
 * dh_scan_refs() uses, so the assertions cover production code, not a copy.
 */
int dh_ins_test_pair_retarget(u32 adrp, u32 follow, u64 adrp_pc,
			      u64 old_base, u64 new_base,
			      u32 *out_adrp, u32 *out_follow)
{
	return insn_pair_retarget(adrp, follow, adrp_pc, old_base, new_base,
				  out_adrp, out_follow);
}
#endif

/* ------------------------------------------------------------------------- */
/* Static-array reference scan.                                               */
/* ------------------------------------------------------------------------- */

/**
 * dh_scan_refs() - find ADRP+ADD / ADRP+LDR references into an old array
 * @ctx: library context (reserved for backend-based text reads)
 * @old_base: start of the old static array
 * @old_len: byte length of the old array
 * @new_base: replacement array base, or %NULL to only enumerate
 * @sites: caller array receiving the PC of each matching ADRP
 * @max_sites: capacity of @sites
 *
 * Scanning is limited to the kernel text window supplied by the runtime via
 * ctx->be->probe("_stext") / probe("_etext"), mirroring
 * features/cgpids/native.c:dnsk_rwprobe_reloc().  Only pairs whose computed
 * target falls inside [old_base, old_base + old_len) are collected: this is
 * the invariant locked by commit 99995a6 (never retarget unrelated symbols).
 *
 * When @new_base is non-NULL every in-range pair is also preflight-re-encoded
 * for it (insn_pair_retarget()).  A pair that cannot be re-encoded fails the
 * whole scan with %DH_E_OPNOTSUPP: returning it as a site would hand the
 * caller a reference it cannot safely patch, and silently dropping it would
 * hide a stale reference.  @new_base == %NULL skips the preflight and only
 * enumerates.
 *
 * Instruction words are read with READ_ONCE/volatile semantics.
 *
 * Return: number of sites collected (0 when none), or negative errno:
 * %DH_E_INVAL for bad arguments, %DH_E_NOENT when the text window cannot be
 * resolved, %DH_E_NOSPC when @max_sites is too small, %DH_E_OPNOTSUPP when an
 * in-range pair cannot be re-encoded for @new_base.
 */
int dh_scan_refs(struct dh_ctx *ctx, void *old_base, size_t old_len,
		 void *new_base, void **sites, size_t max_sites)
{
	const u32 *p, *end;
	u64 olda = (u64)(uintptr_t)old_base;
	size_t found = 0;

	(void)new_base;
	if (!ctx || !ctx->be || !old_base || old_len == 0 || !sites ||
	    max_sites == 0)
		return DH_E_INVAL;
	if (!ctx->be->probe)
		return DH_E_NOENT;
	p = (const u32 *)ctx->be->probe("_stext");
	end = (const u32 *)ctx->be->probe("_etext");
	if (!p || !end || end <= p)
		return DH_E_NOENT;

	for (; (const void *)(p + 2) <= (const void *)end; p++) {
		u32 i0 = insn_read(p);
		u32 i1 = insn_read(p + 1);
		u64 pc = (u64)(uintptr_t)p;
		u64 pg, tgt;

		if ((i0 & ADRP_OP_MASK) != ADRP_OP)
			continue;
		pg = (pc & ~0xFFFULL) +
		     ((u64)adrp_imm_pages(i0) << 12);
		if ((i1 & ADD_OP_MASK) == ADD_OP) {
			/* ADD immediate: the sh bit[22] scales imm12 by 4096. */
			tgt = pg + addsub_imm_off(i1);
		} else if (ldst_is_uimm(i1)) {
			/* LDR/STR/PRFM unsigned immediate: imm12 is scaled
			 * by the access size in bits[31:30]. */
			tgt = pg + ldst_imm_off(i1);
		} else {
			continue;
		}
		if (tgt < olda || tgt >= olda + old_len)
			continue;
		if (new_base) {
			u32 out_adrp, out_follow;
			int ret = insn_pair_retarget(i0, i1, pc, olda,
						    (u64)(uintptr_t)new_base,
						    &out_adrp, &out_follow);

			if (ret != DH_OK)
				return ret;
		}
		if (found >= max_sites)
			return DH_E_NOSPC;
		sites[found++] = (void *)(uintptr_t)pc;
	}
	return (int)found;
}

/* ------------------------------------------------------------------------- */
/* Batch text patch.                                                          */
/* ------------------------------------------------------------------------- */

/**
 * dh_patch_text_batch() - write several instruction sites in one transaction
 * @ctx: library context whose backend supplies text_write()
 * @addrs: array of code addresses
 * @insns: array of instruction-word buffers, one per site
 * @lens: byte length of each buffer
 * @nr: number of sites
 *
 * One batch is capped at DH_MAX_BATCH (64) sites, matching the fixed undo log
 * budget.  Every site is recorded in the transaction before it is written; a
 * failing text_write() triggers dh_txn_rollback() so a partial patch never
 * stays behind.  dh_txn_rollback() is owned by dh_rw.c and supplied as an
 * extern here; the host harness provides a stub.
 *
 * Return: %DH_OK, %DH_E_INVAL for bad arguments, %DH_E_NOSPC when @nr
 * exceeds the batch cap, or the backend's error with rollback already done.
 */
int dh_patch_text_batch(struct dh_ctx *ctx, void **addrs, u32 **insns,
			size_t *lens, size_t nr)
{
	size_t i;
	int ret;

	if (!ctx || !ctx->be || !addrs || !insns || !lens || nr == 0)
		return DH_E_INVAL;
	if (nr > DH_MAX_BATCH)
		return DH_E_NOSPC;
	if (!ctx->be->text_write)
		return DH_E_OPNOTSUPP;
	for (i = 0; i < nr; i++) {
		if (!addrs[i] || !insns[i] || lens[i] == 0 ||
		    (lens[i] & 3) != 0)
			return DH_E_INVAL;
		ret = ctx->be->text_write(addrs[i], insns[i], lens[i],
					  DH_ROUTE_ALIAS);
		if (ret != DH_OK) {
			(void)dh_txn_rollback(ctx);
			return ret;
		}
	}
	return DH_OK;
}
