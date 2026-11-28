"""Verify the AArch64 ADRP+ADD/LDR decode/encode and the rw_reloc reference
pair decision used by features/cgpids/native.c.

Two layers:

1. Ground truth -- a tiny arm64 object is assembled with clang and the real
   instruction words are read back from objdump, so the expectations are
   anchored to what the compiler actually emits.
2. The real C code -- the ``DNSK_AARCH64_PURE_BEGIN``/``END`` block is
   extracted verbatim from native.c, compiled with the host cc and driven
   through ctypes.  This exercises the exact functions the module runs
   (including ``dnsk_reloc_pair``), not a Python re-implementation, so a
   change to a mask or a range check in native.c makes these tests fail.
"""

import ctypes
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
NATIVE_C = ROOT / "features" / "cgpids" / "native.c"
PURE_BEGIN = "/* DNSK_AARCH64_PURE_BEGIN */"
PURE_END = "/* DNSK_AARCH64_PURE_END */"

# enum values mirrored from native.c
REF_NONE, REF_ADD, REF_LDR = 0, 1, 2
RELOC_NONE, RELOC_PATCH, RELOC_FAR = 0, 1, 2

ASM = """
.text
.globl _start
_start:
    adrp x0, target
    add  x0, x0, :lo12:target
    adrp x2, target
    ldr  x3, [x2, #8]
    adrp x4, other
    add  x4, x4, :lo12:other
    ret
.data
.align 3
target: .quad 0x1122334455667788
.skip 0x1000
other: .quad 0x99
"""


def sext(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


# ---- Python reference model (kept in sync with native.c's pure block) -------

def decode_adrp(insn, pc):
    assert (insn & 0x9F000000) == 0x90000000
    imm = ((((insn >> 5) & 0x7FFFF) << 2) | ((insn >> 29) & 3))
    return (pc & ~0xFFF) + (sext(imm, 21) << 12)


def decode_add(insn, page):
    assert (insn & 0xFF800000) == 0x91000000
    shift = 12 if ((insn >> 22) & 3) == 1 else 0
    return page + (((insn >> 10) & 0xFFF) << shift)


def decode_ldr(insn, base):
    assert (insn & 0xFFC00000) == 0xF9400000
    return base + (((insn >> 10) & 0xFFF) * 8)


def repack_adrp(insn, pc, page):
    delta = (page - (pc & ~0xFFF)) >> 12
    lo = delta & 3
    hi = (delta >> 2) & 0x7FFFF
    return (insn & ~0x60FFFFE0) | (lo << 29) | (hi << 5)


def repack_add(insn, addr):
    # imm12 bits[21:10], sh bit22 forced to 0 (byte page offset).
    off = addr & 0xFFF
    return (insn & ~(0x003FFC00 | 0x00400000)) | ((off & 0xFFF) << 10)


def repack_ldr(insn, addr):
    off = addr & 0xFFF
    return (insn & ~0x003FFC00) | (((off >> 3) & 0xFFF) << 10)


def repack_sub(insn, addr, is_add):
    return repack_add(insn, addr) if is_add else repack_ldr(insn, addr)


def ref_pair(pc, i0, i1):
    """Return (kind, target) with kind in {REF_NONE, REF_ADD, REF_LDR}."""
    if (i0 & 0x9F000000) != 0x90000000:
        return REF_NONE, None
    page = decode_adrp(i0, pc)
    if (i1 & 0xFF800000) == 0x91000000:
        return REF_ADD, decode_add(i1, page)
    if (i1 & 0xFFC00000) == 0xF9400000:
        return REF_LDR, decode_ldr(i1, page)
    return REF_NONE, None


# ---- Real-C harness --------------------------------------------------------

def extract_pure_block():
    src = NATIVE_C.read_text()
    i = src.find(PURE_BEGIN)
    j = src.find(PURE_END)
    if i < 0 or j < 0 or j <= i:
        raise AssertionError(
            "DNSK_AARCH64_PURE markers missing/ordered wrong in native.c")
    return src[i + len(PURE_BEGIN):j]


def _setup_prototypes(lib):
    u32, u64 = ctypes.c_uint32, ctypes.c_uint64
    lib.dnsk_insn_is_adrp.argtypes = [u32]
    lib.dnsk_insn_is_adrp.restype = ctypes.c_bool
    lib.dnsk_adrp_target_page.argtypes = [u64, u32]
    lib.dnsk_adrp_target_page.restype = u64
    lib.dnsk_adrp_page.argtypes = [u32, u64, u64]
    lib.dnsk_adrp_page.restype = u32
    lib.dnsk_insn_is_add_imm.argtypes = [u32]
    lib.dnsk_insn_is_add_imm.restype = ctypes.c_bool
    lib.dnsk_insn_is_ldr_uimm64.argtypes = [u32]
    lib.dnsk_insn_is_ldr_uimm64.restype = ctypes.c_bool
    lib.dnsk_add_imm_target.argtypes = [u32, u64]
    lib.dnsk_add_imm_target.restype = u64
    lib.dnsk_ldr_uimm_target.argtypes = [u32, u64]
    lib.dnsk_ldr_uimm_target.restype = u64
    lib.dnsk_repack_add_imm.argtypes = [u32, u32]
    lib.dnsk_repack_add_imm.restype = u32
    lib.dnsk_repack_ldr_uimm64.argtypes = [u32, u32]
    lib.dnsk_repack_ldr_uimm64.restype = u32
    lib.dnsk_ref_pair.argtypes = [u64, u32, u32, ctypes.POINTER(u64)]
    lib.dnsk_ref_pair.restype = ctypes.c_int
    lib.dnsk_ref_in_window.argtypes = [u64, u64]
    lib.dnsk_ref_in_window.restype = ctypes.c_bool
    lib.dnsk_reloc_pair.argtypes = [u64, u32, u32, u64, u64,
                                    ctypes.POINTER(u32)]
    lib.dnsk_reloc_pair.restype = ctypes.c_int


_STATE = {}


def setUpModule():
    tmp = tempfile.TemporaryDirectory()
    _STATE["tmp"] = tmp
    d = pathlib.Path(tmp.name)

    # --- assemble the ground-truth fixture ---
    (d / "t.s").write_text(ASM)
    subprocess.run(["clang", "--target=aarch64-linux-gnu", "-c",
                    str(d / "t.s"), "-o", str(d / "t.o")], check=True)
    subprocess.run(["ld.lld", "-Ttext=0x10000000", "-Tdata=0x30000000",
                    str(d / "t.o"), "-o", str(d / "t.elf")], check=True)
    dis = subprocess.run(["aarch64-linux-gnu-objdump", "-d", str(d / "t.elf")],
                         check=True, capture_output=True, text=True).stdout
    insns = []
    for line in dis.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)", line)
        if m:
            insns.append((int(m.group(1), 16), int(m.group(2), 16),
                          m.group(3)))
    symtxt = subprocess.run(["aarch64-linux-gnu-objdump", "-t",
                             str(d / "t.elf")],
                            check=True, capture_output=True, text=True).stdout
    syms = {}
    for line in symtxt.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1] in ("target", "other", "_start"):
            syms[parts[-1]] = int(parts[0], 16)
    _STATE["insns"] = insns
    _STATE["syms"] = syms

    # --- compile the pure block straight out of native.c ---
    block = extract_pure_block().replace("static inline ", "")
    prelude = ("#include <stdint.h>\n#include <stdbool.h>\n"
               "typedef uint32_t u32;\ntypedef uint64_t u64;\n"
               "typedef int64_t s64;\n")
    (d / "shim.c").write_text(prelude + block)
    so = d / "librefs.so"
    r = subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
                        "-o", str(so), str(d / "shim.c")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError("pure block failed to compile:\n" + r.stderr)
    lib = ctypes.CDLL(str(so))
    _setup_prototypes(lib)
    _STATE["lib"] = lib


def tearDownModule():
    _STATE["tmp"].cleanup()


class _Base(unittest.TestCase):
    @property
    def lib(self):
        return _STATE["lib"]

    @property
    def insns(self):
        return _STATE["insns"]

    @property
    def syms(self):
        return _STATE["syms"]

    @property
    def target(self):
        return _STATE["syms"].get("target")

    def pair(self, follower):
        for i, (pc, insn, mnem) in enumerate(self.insns):
            if mnem != "adrp" or i + 1 >= len(self.insns):
                continue
            npc, ninsn, nmnem = self.insns[i + 1]
            if nmnem == follower:
                return pc, insn, ninsn
        self.fail(f"no adrp+{follower} pair")


class Aarch64RefsTest(_Base):
    """Ground-truth decode/repack against real compiler output (Python model)."""

    def test_symbol_found(self):
        self.assertIsNotNone(self.target)

    def test_decode_add_reaches_symbol(self):
        pc, adrp, add = self.pair("add")
        self.assertEqual(decode_add(add, decode_adrp(adrp, pc)), self.target)

    def test_decode_ldr_reaches_symbol_plus_offset(self):
        pc, adrp, ldr = self.pair("ldr")
        self.assertEqual(decode_ldr(ldr, decode_adrp(adrp, pc)),
                         self.target + 8)

    def test_repack_add_to_new_page(self):
        pc, adrp, add = self.pair("add")
        new = 0x40000008
        nadrp = repack_adrp(adrp, pc, new & ~0xFFF)
        nadd = repack_add(add, new)
        self.assertEqual(decode_adrp(nadrp, pc), new & ~0xFFF)
        self.assertEqual(decode_add(nadd, decode_adrp(nadrp, pc)), new)

    def test_repack_ldr_to_new_page(self):
        pc, adrp, ldr = self.pair("ldr")
        new = 0x40000010
        nadrp = repack_adrp(adrp, pc, new & ~0xFFF)
        nldr = repack_ldr(ldr, new)
        self.assertEqual(decode_adrp(nadrp, pc), new & ~0xFFF)
        self.assertEqual(decode_ldr(nldr, decode_adrp(nadrp, pc)), new)

    def scan_patch(self, old, new):
        out = {}
        for i, (pc, insn, mnem) in enumerate(self.insns):
            if mnem != "adrp" or i + 1 >= len(self.insns):
                continue
            npc, ninsn, nmnem = self.insns[i + 1]
            if nmnem not in ("add", "ldr"):
                continue
            page = decode_adrp(insn, pc)
            tgt = (decode_add(ninsn, page) if nmnem == "add"
                   else decode_ldr(ninsn, page))
            if not old <= tgt < old + 64:
                continue
            na = new + (tgt - old)
            out[pc] = repack_adrp(insn, pc, na & ~0xFFF)
            out[npc] = repack_sub(ninsn, na, nmnem == "add")
        return out

    def test_scan_patches_array_refs_only(self):
        new = 0x40000000
        patched = self.scan_patch(self.target, new)
        pairs = []
        for i, (pc, insn, mnem) in enumerate(self.insns):
            if mnem != "adrp" or i + 1 >= len(self.insns):
                continue
            npc, ninsn, nmnem = self.insns[i + 1]
            if nmnem not in ("add", "ldr"):
                continue
            page = decode_adrp(insn, pc)
            tgt = (decode_add(ninsn, page) if nmnem == "add"
                   else decode_ldr(ninsn, page))
            pairs.append((pc, npc, tgt, nmnem))
        target_pairs = [p for p in pairs
                        if self.target <= p[2] < self.target + 64]
        self.assertEqual(len(target_pairs), 2)
        self.assertEqual(len(patched), 4)
        for pc, npc, tgt, nmnem in target_pairs:
            na = new + (tgt - self.target)
            self.assertEqual(decode_adrp(patched[pc], pc), na & ~0xFFF)
            base = decode_adrp(patched[pc], pc)
            got = (decode_add(patched[npc], base) if nmnem == "add"
                   else decode_ldr(patched[npc], base))
            self.assertEqual(got, na)
        for pc, npc, tgt, _ in pairs:
            if self.syms["other"] <= tgt < self.syms["other"] + 64:
                self.assertNotIn(pc, patched)
                self.assertNotIn(npc, patched)


class PureCRefsTest(_Base):
    """Drive the exact C helpers extracted from native.c through ctypes."""

    def _adrp_pairs(self):
        for i, (pc, insn, mnem) in enumerate(self.insns):
            if mnem != "adrp" or i + 1 >= len(self.insns):
                continue
            npc, ninsn, nmnem = self.insns[i + 1]
            if nmnem in ("add", "ldr"):
                yield pc, insn, npc, ninsn, nmnem

    def test_all_symbols_exported(self):
        for name in ("dnsk_insn_is_adrp", "dnsk_adrp_target_page",
                     "dnsk_adrp_page", "dnsk_insn_is_add_imm",
                     "dnsk_insn_is_ldr_uimm64", "dnsk_add_imm_target",
                     "dnsk_ldr_uimm_target", "dnsk_repack_add_imm",
                     "dnsk_repack_ldr_uimm64", "dnsk_ref_pair",
                     "dnsk_ref_in_window", "dnsk_reloc_pair"):
            self.assertTrue(hasattr(self.lib, name), name)

    def test_c_adrp_decode_matches_python_and_real(self):
        seen = 0
        for pc, adrp, _npc, ninsn, nmnem in self._adrp_pairs():
            cpage = self.lib.dnsk_adrp_target_page(pc, adrp)
            self.assertEqual(cpage, decode_adrp(adrp, pc))
            if nmnem == "add":
                self.assertEqual(
                    self.lib.dnsk_add_imm_target(ninsn, cpage),
                    decode_add(ninsn, cpage))
            else:
                self.assertEqual(
                    self.lib.dnsk_ldr_uimm_target(ninsn, cpage),
                    decode_ldr(ninsn, cpage))
            seen += 1
        self.assertGreaterEqual(seen, 3)

    def test_c_instruction_classifiers(self):
        for _pc, adrp, _npc, ninsn, nmnem in self._adrp_pairs():
            self.assertTrue(self.lib.dnsk_insn_is_adrp(adrp))
            if nmnem == "add":
                self.assertTrue(self.lib.dnsk_insn_is_add_imm(ninsn))
                self.assertFalse(self.lib.dnsk_insn_is_ldr_uimm64(ninsn))
            else:
                self.assertTrue(self.lib.dnsk_insn_is_ldr_uimm64(ninsn))
                self.assertFalse(self.lib.dnsk_insn_is_add_imm(ninsn))
        # a plain RET is none of these
        self.assertFalse(self.lib.dnsk_insn_is_adrp(0xD65F03C0))
        self.assertFalse(self.lib.dnsk_insn_is_add_imm(0xD65F03C0))
        self.assertFalse(self.lib.dnsk_insn_is_ldr_uimm64(0xD65F03C0))

    def test_python_and_c_ref_pair_agree(self):
        for pc, adrp, _npc, ninsn, _nmnem in self._adrp_pairs():
            tgt = ctypes.c_uint64(0)
            kind = self.lib.dnsk_ref_pair(pc, adrp, ninsn,
                                          ctypes.byref(tgt))
            pkind, ptgt = ref_pair(pc, adrp, ninsn)
            self.assertEqual(kind, pkind)
            if kind != REF_NONE:
                self.assertEqual(tgt.value, ptgt)

    def test_c_repack_add_roundtrip_real(self):
        pc, adrp, npc, add, _ = next(
            p for p in self._adrp_pairs() if p[4] == "add")
        new = 0x40000008
        nadrp = self.lib.dnsk_adrp_page(adrp, pc, new & ~0xFFF)
        nadd = self.lib.dnsk_repack_add_imm(add, new & 0xFFF)
        self.assertEqual(self.lib.dnsk_adrp_target_page(pc, nadrp),
                         new & ~0xFFF)
        self.assertEqual(
            self.lib.dnsk_add_imm_target(nadd,
                                         self.lib.dnsk_adrp_target_page(
                                             pc, nadrp)),
            new)

    def test_c_repack_ldr_roundtrip_real(self):
        pc, adrp, npc, ldr, _ = next(
            p for p in self._adrp_pairs() if p[4] == "ldr")
        new = 0x40000010
        nadrp = self.lib.dnsk_adrp_page(adrp, pc, new & ~0xFFF)
        nldr = self.lib.dnsk_repack_ldr_uimm64(ldr, new & 0xFFF)
        self.assertEqual(
            self.lib.dnsk_ldr_uimm_target(nldr,
                                          self.lib.dnsk_adrp_target_page(
                                              pc, nadrp)),
            new)

    def test_add_repack_precise_mask_and_forced_shift(self):
        # sh == 0 ADD: imm12 at bits[21:10], bit22/bit23 stay 0.
        base = 0x91000000 | (0x123 << 10) | 0x1F  # Rn=0, Rd=31
        out = self.lib.dnsk_repack_add_imm(base, 0x456)
        self.assertEqual(out, (base & ~(0x003FFC00 | 0x00400000))
                         | (0x456 << 10))
        self.assertEqual((out >> 10) & 0xFFF, 0x456)
        self.assertEqual((out >> 22) & 1, 0)
        self.assertEqual((out >> 23) & 1, 0)
        # everything outside imm12/sh unchanged
        self.assertEqual(out & ~(0x003FFC00 | 0x00400000),
                         base & ~(0x003FFC00 | 0x00400000))

        # sh == 1 ADD: repack must FORCE sh to 0 (off is a byte page offset).
        sh1 = 0x91000000 | (1 << 22) | (0x005 << 10) | 0x1F
        self.assertEqual((sh1 >> 22) & 1, 1)
        out1 = self.lib.dnsk_repack_add_imm(sh1, 0x033)
        self.assertEqual((out1 >> 22) & 1, 0)
        self.assertEqual((out1 >> 10) & 0xFFF, 0x033)
        # a naive imm12-only mask would have left sh=1 set -> mis-encode.
        naive = (sh1 & ~0x003FFC00) | (0x033 << 10)
        self.assertEqual((naive >> 22) & 1, 1)

    def test_add_repack_identical_to_old_wide_mask_for_sh0(self):
        # The old code used ~0x00FFFC00; for every real ADD (sh==0, bit23==0)
        # the precise form must be byte-identical, so default behaviour is
        # unchanged.
        adds = [ninsn for _, _, _, ninsn, m in self._adrp_pairs()
                if m == "add"]
        self.assertTrue(adds)
        for add in adds:
            self.assertEqual((add >> 22) & 1, 0)
            self.assertEqual((add >> 23) & 1, 0)
            for off in (0x000, 0x008, 0x123, 0xFF8, 0xFFF):
                precise = self.lib.dnsk_repack_add_imm(add, off)
                legacy = (add & ~0x00FFFC00) | ((off & 0xFFF) << 10)
                self.assertEqual(precise, legacy)

    def test_ref_in_window_boundaries(self):
        base = 0x1000
        w = self.lib.dnsk_ref_in_window
        self.assertTrue(w(base, base))
        self.assertTrue(w(base + 63, base))
        self.assertFalse(w(base + 64, base))     # exclusive end (catches off-by-one)
        self.assertFalse(w(base - 1, base))

    def test_reloc_pair_patches_real_pairs(self):
        olda = self.target
        neua = 0x40000000 + (olda & 0xFFF)
        for pc, adrp, _npc, ninsn, _nmnem in self._adrp_pairs():
            kind, tgt = ref_pair(pc, adrp, ninsn)
            buf = (ctypes.c_uint32 * 2)()
            res = self.lib.dnsk_reloc_pair(pc, adrp, ninsn, olda, neua, buf)
            if tgt is not None and olda <= tgt < olda + 64:
                self.assertEqual(res, RELOC_PATCH)
                na = neua + (tgt - olda)
                self.assertEqual(self.lib.dnsk_adrp_target_page(pc, buf[0]),
                                 na & ~0xFFF)
                base = self.lib.dnsk_adrp_target_page(pc, buf[0])
                got = (self.lib.dnsk_add_imm_target(buf[1], base)
                       if kind == REF_ADD
                       else self.lib.dnsk_ldr_uimm_target(buf[1], base))
                self.assertEqual(got, na)
            else:
                self.assertEqual(res, RELOC_NONE)

    def test_reloc_pair_ignores_non_adrp_first_word(self):
        buf = (ctypes.c_uint32 * 2)()
        res = self.lib.dnsk_reloc_pair(0x1000, 0xD65F03C0, 0x91000000,
                                       0x2000, 0x4000, buf)
        self.assertEqual(res, RELOC_NONE)

    def test_reloc_pair_window_end_is_exclusive(self):
        # ADRP to own page (delta 0) => target == pc page; ADD #off reaches any
        # byte offset.  Window is [base, base+64): 63 is in, 64 is out.
        pc = 0x40000000
        adrp = 0x90000000
        base = pc & ~0xFFF
        for off, expect in ((0, RELOC_PATCH), (63, RELOC_PATCH),
                            (64, RELOC_NONE), (0x3F, RELOC_PATCH),
                            (0x40, RELOC_NONE)):
            add = 0x91000000 | (off << 10)
            buf = (ctypes.c_uint32 * 2)()
            res = self.lib.dnsk_reloc_pair(pc, adrp, add, base,
                                           0x80000000, buf)
            self.assertEqual(res, expect, f"off={off:#x}")

    def test_reloc_pair_far_target_is_rejected(self):
        pc = 0x40000000
        adrp = 0x90000000            # page = pc page
        add = 0x91000000             # off 0 -> target == pc page
        olda = pc & ~0xFFF
        neua = olda + (5 << 30)      # ~5 GiB away, beyond ADRP +/-4 GiB
        buf = (ctypes.c_uint32 * 2)()
        res = self.lib.dnsk_reloc_pair(pc, adrp, add, olda, neua, buf)
        self.assertEqual(res, RELOC_FAR)

    def test_residual_detection_catches_a_missed_pair(self):
        # Emulate the kernel's post-reloc verification sweep on a synthetic
        # text stream: it must read 0 after retargeting all pairs, and >0 if
        # one pair is left pointing at the old array.
        olda = 0x50000000
        neua = 0x60000000

        def make_pair(pc, off):
            # ADRP at pc -> olda's page; ADD #off -> olda + off.
            adrp = repack_adrp(0x90000000, pc, olda & ~0xFFF)
            add = 0x91000000 | ((off & 0xFFF) << 10)
            return [pc, adrp, add]

        stream = [make_pair(0x50010000, 0),
                  make_pair(0x50010008, 16),
                  make_pair(0x50010010, 0x800)]  # 0x800 is outside the window

        def residual(words):
            n = 0
            for pc, i0, i1 in words:
                tgt = ctypes.c_uint64(0)
                kind = self.lib.dnsk_ref_pair(pc, i0, i1,
                                              ctypes.byref(tgt))
                if kind != REF_NONE and self.lib.dnsk_ref_in_window(
                        tgt.value, olda):
                    n += 1
            return n

        self.assertEqual(residual(stream), 2)

        patched = [list(w) for w in stream]
        for pc, adrp, add in stream:
            buf = (ctypes.c_uint32 * 2)()
            res = self.lib.dnsk_reloc_pair(pc, adrp, add, olda, neua, buf)
            if res == RELOC_PATCH:
                self._apply(patched, pc, buf)
        self.assertEqual(residual(patched), 0)

        # Missed patch: retarget only the first in-window pair.
        missed = [list(w) for w in stream]
        buf = (ctypes.c_uint32 * 2)()
        pc0 = stream[0][0]
        self.assertEqual(
            self.lib.dnsk_reloc_pair(pc0, stream[0][1], stream[0][2],
                                     olda, neua, buf), RELOC_PATCH)
        self._apply(missed, pc0, buf)
        self.assertEqual(residual(missed), 1)

    @staticmethod
    def _apply(words, pc, buf):
        for w in words:
            if w[0] == pc:
                w[1], w[2] = buf[0], buf[1]


if __name__ == "__main__":
    unittest.main()
