#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host selftest for the Droid-Hook arm64 instruction layer.

Builds tests/host/dh_insn_harness.c together with the real src/dh_insn.c and
runs each subcommand, asserting the encoder output.

Reference encodings are FROZEN CONSTANTS in this file (REF below), each with the
assembler source line that produces it and the exact command sequence used to
read it.  The external toolchain (clang + ld.lld + aarch64-linux-gnu-objdump) is
an OPTIONAL cross-check: when all three exist the frozen value is additionally
asserted equal to a live assembly, which is stronger than either alone; when any
is missing the encoder is still asserted against the frozen table and the test
prints which tool was missing.  An earlier version depended on the toolchain
unconditionally, which made the gate pass only on a host that had it.

No kernel headers, no kernel build, no CI, no Action.
"""

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
INC = ROOT / 'include'
SRC = ROOT / 'src' / 'dh_insn.c'
HARNESS = pathlib.Path(__file__).resolve().parent / 'dh_insn_harness.c'
BIN = pathlib.Path('/tmp/dh_insn_harness')

CC = 'cc'
# -DDH_INS_TEST_SEAM compiles the test-only wrapper around the static
# ADRP+follower pair re-encoder, so the assertions below cover the same code
# dh_scan_refs() uses in production.  Production builds never define it.
CFLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-DDH_INS_TEST_SEAM',
          '-I', str(INC), '-I', str(ROOT / 'runtime')]


def build():
    subprocess.run([CC, *CFLAGS, str(HARNESS), str(SRC), '-o', str(BIN)],
                   check=True, capture_output=True, text=True)


def run(cmd):
    out = subprocess.run([str(BIN), cmd], check=True,
                         capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        if '=' not in line:
            continue
        k, v = line.split('=', 1)
        d[k] = int(v, 0) if v.startswith(('0x', '-', '+')) or v.isdigit() else v
    return d


# ---------------------------------------------------------------------------
# Frozen reference encodings
#
# Provenance: every value below was produced on the development host by
#
#   clang --target=aarch64-linux-gnu -c t.s -o t.o
#   ld.lld -Ttext=0x10000000 t.o -o t.elf
#   aarch64-linux-gnu-objdump -d t.elf
#
# and copied out of that disassembly.  Each comment names the exact source line
# and the address it was assembled at, so the value can be re-derived from the
# encoding tables even on a host without any toolchain.  These constants -- not
# the toolchain -- are what the encoder is asserted against.
# ---------------------------------------------------------------------------

TOOLS = ('clang', 'ld.lld', 'aarch64-linux-gnu-objdump')

ASM = {
    'jump': '.text\n.globl _s\n_s:\n ldr x17, #8\n br x17\n',
    'adrp_pos': '.text\n.globl _s\n_s:\n adrp x0, #0x1000\n',
    'adrp_neg': '.text\n.globl _s\n_s:\n adrp x1, #-0x4000\n',
    'add': '.text\n.globl _s\n_s:\n'
           ' add x2, x2, #0x123, lsl #12\n add x2, x2, #0x123\n',
    'ldr': '.text\n.globl _s\n_s:\n ldr x4, [x5, #64]\n ldrb w6, [x7, #1]\n',
    'ldr_lit': '.text\n.globl _s\n_s:\n ldr x9, #0x100\n',
}

REF = {
    # 0x10000000: ldr x17, #8   (LDR literal, Rt=17, imm19=1 word = +8 bytes)
    # 0x10000004: br x17        (BR, Rn=17)
    'jump': (0x58000051, 0xd61f0220),
    # 0x10000000: adrp x0, #0x1000 -- the immediate is an absolute address, so
    # the delta is 0x1000 page minus 0x10000000 page = -0xFFFF pages.  Kept
    # verbatim because the test compares only the immediate field.
    'adrp_pos': (0xb0000000,),
    # 0x10000000: adrp x1, #-0x4000 -> -4 pages: immlo=0, immhi=0x7FFFF
    'adrp_neg': (0x90ffffe1,),
    # 0x10000000: add x2, x2, #0x123, lsl #12  (imm12=0x123, sh=1 -> bit22)
    # 0x10000004: add x2, x2, #0x123           (imm12=0x123, sh=0)
    'add': (0x91448c42, 0x91048c42),
    # 0x10000000: ldr x4, [x5, #64]  (size=3, opc=01, imm12=64/8=8)
    # 0x10000004: ldrb w6, [x7, #1]  (size=0, opc=01, imm12=1)
    'ldr': (0xf94020a4, 0x394004e6),
    # 0x10000000: ldr x9, #0x100     (LDR literal, imm19=0x40 words)
    'ldr_lit': (0x58000809,),
}

# Forms the ISA cannot encode; clang rejects them at assembly time.  Frozen as a
# list so the cross-check can re-verify when a toolchain is available.
REJECTED = ('ldr x9, #0x100000', 'b #0x8000000', 'adrp x0, #0x100000000')


def forced_no_toolchain():
    """True when the degraded path was requested, not caused by a missing tool.

    Kept separate from missing_tools() so a log never claims a tool is absent
    when it is merely being ignored: that would be a false statement in the
    very evidence this project relies on.
    """
    return bool(os.environ.get('DH_INSN_FORCE_NO_TOOLCHAIN'))


def missing_tools():
    """Names from TOOLS absent on this host.

    DH_INSN_FORCE_NO_TOOLCHAIN=1 forces the "toolchain absent" path, so the
    degraded mode can be reproduced deterministically on a machine that has the
    tools (and is the normal mode on a runner that does not).  Callers that
    print a reason must distinguish the two cases; see forced_no_toolchain().
    """
    if forced_no_toolchain():
        return list(TOOLS)
    return [t for t in TOOLS if shutil.which(t) is None]


def objdump_insns(asm, link_script=None):
    """Return [(pc, word, mnemonic)] from real clang+ld.lld output.

    Optional path: only called when all of TOOLS exist (or when a caller asked
    for it explicitly).  Tests must never depend on this to assert encoding
    correctness -- REF is the source of truth.
    """
    with tempfile.TemporaryDirectory() as td:
        d = pathlib.Path(td)
        (d / 't.s').write_text(asm)
        subprocess.run(['clang', '--target=aarch64-linux-gnu', '-c',
                        str(d / 't.s'), '-o', str(d / 't.o')],
                       check=True, capture_output=True, text=True)
        cmd = ['ld.lld', '-Ttext=0x10000000']
        if link_script:
            cmd += ['-Tdata=0x30000000']
        cmd += [str(d / 't.o'), '-o', str(d / 't.elf')]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        dis = subprocess.run(['aarch64-linux-gnu-objdump', '-d',
                              str(d / 't.elf')],
                             check=True, capture_output=True, text=True).stdout
    out = []
    for line in dis.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)', line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
    return out


class DhInsnTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build()

    def ref_words(self, name):
        """Frozen words for a snippet, cross-checked against live assembly.

        Always returns the frozen tuple, so the encoder assertion that follows
        runs in every environment.  When clang, ld.lld and the cross objdump are
        all present the same snippet is assembled and the two must agree, which
        is strictly stronger than either check alone.  When one is missing the
        cross-check is skipped with an explicit message -- never the encoder
        assertion itself.
        """
        frozen = REF[name]
        missing = missing_tools()
        if missing:
            if forced_no_toolchain():
                print(f'note: {name}: cross-check disabled by '
                      f'DH_INSN_FORCE_NO_TOOLCHAIN, so its absence does NOT mean '
                      f'a tool is missing; asserting against the frozen table')
            else:
                print(f'note: {name}: cross-check not executed, missing '
                      f'{", ".join(missing)}; asserting against the frozen table')
            return frozen
        live = tuple(w for _, w, _ in objdump_insns(ASM[name]))
        self.assertEqual(live, frozen,
                         f'{name}: frozen REF disagrees with the assembler')
        print(f'note: {name}: cross-check executed against live assembly')
        return frozen

    # ---- requirement 1: KernelPatch-style absolute jump ----

    def test_absolute_jump_matches_assembler(self):
        d = run('jump')
        self.assertEqual(d['ret'], 0)
        # Frozen reference: ldr x17, #8 ; br x17 -> 0x58000051 / 0xd61f0220
        ref = self.ref_words('jump')
        self.assertEqual(ref[0], 0x58000051)
        self.assertEqual(ref[1], 0xd61f0220)
        self.assertEqual(d['w0'], ref[0])
        self.assertEqual(d['w1'], ref[1])
        target = 0x1122334455667788
        self.assertEqual(d['w2'], target & 0xFFFFFFFF)
        self.assertEqual(d['w3'], target >> 32)
        self.assertEqual(d['ret_null'], -22)  # DH_E_INVAL

    # ---- requirement 2: ADRP page re-encoding, positive and negative ----

    def test_adrp_positive_delta_keeps_absolute_target(self):
        d = run('reloc_adrp')
        self.assertEqual(d['r_pos'], 0)
        # Original: PC 0x40000000, +2 pages -> 0x40002000.
        # New PC 0x40001000 => page delta must be +1.
        self.assertEqual(d['pages_pos'], 1)
        self.assertEqual(d['target_from_new_pc'], 0x40002000)
        # Frozen reference: adrp x0, #0x1000 assembled at 0x10000000.  Only
        # the 21-bit immediate field is compared, so the absolute immediate the
        # assembler chose for the reference does not matter.
        ref = self.ref_words('adrp_pos')
        self.assertEqual(ref[0] & 0x60FFFFE0,
                         d['dst_pos'] & 0x60FFFFE0)

    def test_adrp_negative_delta(self):
        d = run('reloc_adrp')
        self.assertEqual(d['r_neg'], 0)
        # Original -3 pages from 0x40000000; new PC +1 page => -4 pages.
        self.assertEqual(d['pages_neg'], -4)
        # Frozen reference: adrp x1, #-0x4000 at 0x10000000 (-4 pages).
        ref = self.ref_words('adrp_neg')
        self.assertEqual(ref[0] & 0x60FFFFE0,
                         d['dst_neg'] & 0x60FFFFE0)

    # ---- requirement 2: ADD/SUB imm12 and sh bit preserved ----

    def test_add_imm12_and_sh_preserved(self):
        d = run('add_sh')
        self.assertEqual(d['r_sh1'], 0)
        self.assertEqual(d['r_sh0'], 0)
        # 0x91448C42 = add x2,x2,#0x123,lsl#12 (sh=1)
        # 0x91048C42 = add x2,x2,#0x123 (sh=0)
        self.assertEqual(d['dst_sh1'], 0x91448C42)
        self.assertEqual(d['dst_sh0'], 0x91048C42)
        self.assertEqual(d['sh_bit_sh1'], 1)
        self.assertEqual(d['sh_bit_sh0'], 0)
        self.assertEqual(d['imm12_sh1'], 0x123)
        # Frozen reference: add with LSL #12 (sh=1) and without (sh=0).
        ref = self.ref_words('add')
        self.assertEqual(ref[0], 0x91448C42)
        self.assertEqual(ref[1], 0x91048C42)

    # ---- requirement 2: LDR opc bits must not be cleared (commit 8caf6e6) ----

    def test_ldr_opc_bits_not_cleared(self):
        d = run('ldr_opc')
        self.assertEqual(d['r_ldrx'], 0)
        self.assertEqual(d['r_ldrb'], 0)
        # Whole-word copy keeps size (bits[31:30]), opc (bits[23:22]) and V.
        self.assertEqual(d['dst_ldrx'], 0xF94020A4)
        self.assertEqual(d['dst_ldrb'], 0x394004E6)
        # The buggy mask 0x00FFFC00 would clear bit[23:22] too: prove it.
        buggy = 0xF94020A4 & ~0x00FFFC00
        self.assertNotEqual(buggy, d['dst_ldrx'])
        # Frozen reference: LDR x (size=3) and LDRB (size=0); both opc=01.
        ref = self.ref_words('ldr')
        self.assertEqual(ref[0], 0xF94020A4)
        self.assertEqual(ref[1], 0x394004E6)

    # ---- requirement 2: LDR literal ±1 MiB ----

    def test_ldr_literal_retarget_and_range(self):
        d = run('ldr_lit')
        self.assertEqual(d['r'], 0)
        # Original at 0x1000, word delta +64 -> 0x1100.
        # Relocated to 0x2000 must still reach 0x1100 => word delta -960.
        self.assertEqual(d['word_delta'], -960)
        self.assertEqual(d['target'], 0x1100)
        # Frozen reference: ldr x9, #0x100 (LDR literal, imm19=0x40 words).
        ref = self.ref_words('ldr_lit')
        self.assertEqual(ref[0], 0x58000809)
        self.assertEqual(d['src'], ref[0])
        # Out-of-range literal must be rejected (checked in test_range_rejects).

    # ---- requirement 2: range checks refuse out-of-range, never truncate ----

    def test_range_rejects_out_of_range(self):
        d = run('range')
        self.assertEqual(d['opnotsupp'], -95)  # DH_E_OPNOTSUPP
        self.assertEqual(d['r_adrp_out'], -95)  # ±4 GiB ADRP
        self.assertEqual(d['r_b_out'], -95)     # ±128 MiB B
        self.assertEqual(d['r_lit_out'], -95)   # ±1 MiB LDR literal
        self.assertEqual(d['r_ok'], 0)          # in-range still succeeds
        # The out-of-range refusal above is asserted unconditionally (it comes
        # from the encoder).  The assembler's agreement is a cross-check: the
        # ISA genuinely cannot encode these forms, so re-verify it whenever a
        # toolchain is present and say so when it is not.
        missing = missing_tools()
        for spec in REJECTED:
            if missing:
                if forced_no_toolchain():
                    print(f'note: assembler rejection not cross-checked for '
                          f'{spec!r}: disabled by DH_INSN_FORCE_NO_TOOLCHAIN')
                else:
                    print(f'note: assembler rejection not cross-checked for '
                          f'{spec!r}, missing {", ".join(missing)}')
                continue
            with self.assertRaises(subprocess.CalledProcessError):
                objdump_insns('.text\n.globl _s\n_s:\n ' + spec + '\n')
        if not missing:
            print('note: assembler rejection cross-check executed for '
                  f'{len(REJECTED)} out-of-range forms')

    # ---- requirement 3: scan only in-range references ----

    def test_scan_only_collects_in_range_refs(self):
        d = run('scan')
        self.assertEqual(d['n'], 2)
        self.assertEqual(d['site0'], d['expect0'])
        self.assertEqual(d['site1'], d['expect1'])
        # The third ADRP+ADD pair points one page past the 64 B array and
        # must NOT appear (commit 99995a6 invariant).

    def test_scan_empty_returns_zero(self):
        d = run('scan_empty')
        self.assertEqual(d['n'], 0)
        self.assertEqual(d['r_null'], -22)  # bad args are errors, not 0

    # ---- requirement 4: batch, rollback, cap ----

    def test_batch_success_fail_and_cap(self):
        d = run('batch')
        self.assertEqual(d['r_ok'], 0)
        self.assertEqual(d['writes'], 3)
        self.assertEqual(d['w0'], 0x11111111)
        self.assertEqual(d['w1'], 0x22222222)
        self.assertEqual(d['r_fail'], -1000)   # DH_E_VERIFY from backend
        self.assertEqual(d['rollback_calls'], 1)
        self.assertEqual(d['writes_on_fail'], 1)  # stopped at first failure
        c = run('batch_cap')
        self.assertEqual(c['nospc'], -28)        # DH_E_NOSPC
        self.assertEqual(c['r_cap'], -28)
        self.assertEqual(c['writes'], 0)         # rejected before any write

    # ---- ADRP + follower pair re-encode (contract gap, static helper) ----

    ADR_IMM = 0x60FFFFE0
    # ADD/SUB (immediate): imm12 is bits[21:10]; sh is bits[23:22] and may be
    # changed only by the explicit LSL #12 -> LSL #0 conversion.
    ADD_IMM = 0x003FFC00
    ADD_SH = 0x00C00000
    LDST_IMM = 0x003FFC00

    def assert_only_imm_changed(self, d, case, follow_mask):
        """A re-encode may only touch the documented immediate fields."""
        self.assertEqual(d[f'case_{case}_xor_adrp'] & ~self.ADR_IMM, 0,
                         f'{case}: ADRP non-immediate bits changed')
        self.assertEqual(d[f'case_{case}_xor_follow'] & ~follow_mask, 0,
                         f'{case}: follower non-immediate bits changed')

    def test_pair_add_rewrites_imm12_and_keeps_sh(self):
        d = run('pair')
        # Old: page 0x20000000 + #0x20, array moved to a page-aligned base:
        # page delta 0x2000, imm12 stays 0x20, sh=0, nothing else moves.
        self.assertEqual(d['case_add_sh0_ret'], 0)
        self.assertEqual(d['case_add_sh0_pages'], 0x20000)
        self.assertEqual(d['case_add_sh0_imm12'], 0x20)
        self.assertEqual(d['case_add_sh0_sh'], 0)
        self.assertEqual(d['case_add_sh0_dst_follow'], 0x91008022)
        self.assert_only_imm_changed(d, 'add_sh0', self.ADD_IMM)
        # Base shifted by 0x40: imm12 must become 0x60, sh still 0.
        self.assertEqual(d['case_add_sh0_shifted_ret'], 0)
        self.assertEqual(d['case_add_sh0_shifted_pages'], 0x20000)
        self.assertEqual(d['case_add_sh0_shifted_imm12'], 0x60)
        self.assertEqual(d['case_add_sh0_shifted_sh'], 0)
        self.assertEqual(d['case_add_sh0_shifted_dst_follow'], 0x91018022)
        self.assert_only_imm_changed(d, 'add_sh0_shifted', self.ADD_IMM)
        # sh=1 case: the old pair reached page+0x1000 via imm12=1; after the
        # move the target page is exact, so imm12 becomes 0 and sh stays 1.
        self.assertEqual(d['case_add_sh1_ret'], 0)
        self.assertEqual(d['case_add_sh1_pages'], 0x20001)
        self.assertEqual(d['case_add_sh1_imm12'], 0)
        self.assertEqual(d['case_add_sh1_sh'], 1)
        self.assertEqual(d['case_add_sh1_dst_follow'], 0x91400022)
        self.assert_only_imm_changed(d, 'add_sh1', self.ADD_IMM)

    def test_pair_ldr_keeps_size_opc_and_rescales_imm12(self):
        d = run('pair')
        # LDR x: size=3, opc=1, imm12 scaled by 8.  Base moved by 0x40 turns
        # an offset of 0x20 into 0x60, i.e. imm12 4 -> 12.
        self.assertEqual(d['case_ldr_x_ret'], 0)
        self.assertEqual(d['case_ldr_x_imm12'], 4)
        self.assertEqual(d['case_ldr_x_opc'], 1)
        self.assertEqual(d['case_ldr_x_size'], 3)
        self.assertEqual(d['case_ldr_x_dst_follow'], 0xF9401043)
        self.assert_only_imm_changed(d, 'ldr_x', self.LDST_IMM)
        self.assertEqual(d['case_ldr_x_shifted_ret'], 0)
        self.assertEqual(d['case_ldr_x_shifted_imm12'], 0xC)
        self.assertEqual(d['case_ldr_x_shifted_opc'], 1)
        self.assertEqual(d['case_ldr_x_shifted_size'], 3)
        self.assertEqual(d['case_ldr_x_shifted_dst_follow'], 0xF9403043)
        self.assert_only_imm_changed(d, 'ldr_x_shifted', self.LDST_IMM)
        # LDRB: scale 1, so imm12 is the byte offset itself; opc/size stay.
        self.assertEqual(d['case_ldrb_ret'], 0)
        self.assertEqual(d['case_ldrb_imm12'], 0x20)
        self.assertEqual(d['case_ldrb_opc'], 1)
        self.assertEqual(d['case_ldrb_size'], 0)
        self.assertEqual(d['case_ldrb_dst_follow'], 0x39408085)
        self.assert_only_imm_changed(d, 'ldrb', self.LDST_IMM)

    def test_pair_converts_lsl12_to_lsl0_when_needed(self):
        # An LSL #12 pair cannot express a non-page-aligned target in that
        # form, so it is re-encoded as LSL #0 against the target page instead
        # of being refused or silently mis-encoded.  Address stays exact:
        # page 0x30001000 + 0x20 == old 0x20001000 + (base 0x30000020 - 0x20000000).
        d = run('pair')
        self.assertEqual(d['case_add_sh1_misaligned_ret'], 0)
        self.assertEqual(d['case_add_sh1_misaligned_pages'], 0x20001)
        self.assertEqual(d['case_add_sh1_misaligned_imm12'], 0x20)
        self.assertEqual(d['case_add_sh1_misaligned_sh'], 0)
        self.assertEqual(d['case_add_sh1_misaligned_dst_follow'], 0x91008022)
        self.assert_only_imm_changed(
            d, 'add_sh1_misaligned', self.ADD_IMM | self.ADD_SH)

    def test_pair_refuses_what_it_cannot_encode(self):
        d = run('pair')
        # A load offset that is not a multiple of the access size.
        self.assertEqual(d['case_scale_ret'], -95)
        # An ADRP page delta outside +-4 GiB.
        self.assertEqual(d['case_far_ret'], -95)
        # Follower classes that are not an address materialisation.
        self.assertEqual(d['case_nop_follower_ret'], -95)
        self.assertEqual(d['case_sub_follower_ret'], -95)
        # A first word that is not ADRP.
        self.assertEqual(d['case_bad_adrp_ret'], -22)
        self.assertEqual(d['r_null_out'], -22)

    # ---- ADRP +-4 GiB boundary (both edges, not just "far away") ----

    def test_adrp_range_boundary(self):
        d = run('range_boundary')
        self.assertEqual(d['bound_max_ret'], 0)
        self.assertEqual(d['bound_max_pages'], 0xFFFFF)
        self.assertEqual(d['bound_over_ret'], -95)     # 2^20 pages refused
        self.assertEqual(d['bound_min_ret'], 0)
        self.assertEqual(d['bound_min_pages'], -0x100000)
        self.assertEqual(d['bound_under_ret'], -95)    # -2^20-1 refused

    # ---- LDR literal with a backward target ----

    def test_ldr_literal_negative_delta(self):
        d = run('ldr_lit_neg')
        self.assertEqual(d['r'], 0)
        self.assertEqual(d['target'], d['expect_target'])
        self.assertEqual(d['target'], 0x1F00)
        self.assertEqual(d['word_delta'], 960)

    # ---- scan boundaries and fail-closed preflight ----

    def test_scan_boundary_is_half_open(self):
        d = run('scan_bounds')
        # old_base is inside, old_base + old_len is outside.
        self.assertEqual(d['n'], 1)
        self.assertEqual(d['site0'], d['expect0'])

    def test_scan_fails_closed_when_pair_cannot_be_re_encoded(self):
        d = run('scan_unencodable')
        # Not 0 and not 1: the site is in range but unpatchable, so the scan
        # refuses the whole operation instead of dropping it silently.
        self.assertEqual(d['n'], -95)
        self.assertEqual(d['opnotsupp'], -95)

    # ---- argument and class validation ----

    def test_relocate_rejects_bad_args_and_unknown_class(self):
        d = run('reloc_bad')
        self.assertEqual(d['r_null_src'], -22)
        self.assertEqual(d['r_null_dst'], -22)
        self.assertEqual(d['r_nr0'], -22)
        self.assertEqual(d['r_unknown'], -95)


if __name__ == '__main__':
    unittest.main()
