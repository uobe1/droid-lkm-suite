"""Verify the AArch64 ADRP+ADD/LDR decode and repack used by rw_reloc."""

import pathlib
import re
import subprocess
import tempfile
import unittest

ASM = """
.text
.globl _start
_start:
    adrp x0, target
    add  x0, x0, :lo12:target
    adrp x2, target
    ldr  x3, [x2, #8]
    ret
.data
.align 3
target: .quad 0x1122334455667788
"""


def sext(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


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


def repack_sub(insn, addr, is_add):
    off = addr & 0xFFF
    imm = off if is_add else off >> 3
    mask = ~0x00FFFC00 if is_add else ~0x003FFC00
    return (insn & mask) | ((imm & 0xFFF) << 10)


class Aarch64RefsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = pathlib.Path(cls.tmp.name)
        (d / "t.s").write_text(ASM)
        subprocess.run(["clang", "--target=aarch64-linux-gnu", "-c",
                        str(d / "t.s"), "-o", str(d / "t.o")], check=True)
        subprocess.run(["ld.lld", "-Ttext=0x10000000", "-Tdata=0x30000000",
                        str(d / "t.o"), "-o", str(d / "t.elf")], check=True)
        dis = subprocess.run(["aarch64-linux-gnu-objdump", "-d",
                              str(d / "t.elf")], check=True,
                             capture_output=True, text=True).stdout
        cls.insns = []
        for line in dis.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)", line)
            if m:
                cls.insns.append((int(m.group(1), 16),
                                  int(m.group(2), 16), m.group(3)))
        syms = subprocess.run(["aarch64-linux-gnu-objdump", "-t",
                               str(d / "t.elf")], check=True,
                              capture_output=True, text=True).stdout
        cls.target = None
        for line in syms.splitlines():
            if line.split()[-1:] == ["target"]:
                cls.target = int(line.split()[0], 16)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_symbol_found(self):
        self.assertIsNotNone(self.target)

    def pair(self, follower):
        for i, (pc, insn, mnem) in enumerate(self.insns):
            if mnem != "adrp" or i + 1 >= len(self.insns):
                continue
            npc, ninsn, nmnem = self.insns[i + 1]
            if nmnem == follower:
                return pc, insn, ninsn
        self.fail("no adrp+%s pair" % follower)

    def test_decode_add_reaches_symbol(self):
        pc, adrp, add = self.pair("add")
        self.assertEqual(decode_add(add, decode_adrp(adrp, pc)),
                         self.target)

    def test_decode_ldr_reaches_symbol_plus_offset(self):
        pc, adrp, ldr = self.pair("ldr")
        self.assertEqual(decode_ldr(ldr, decode_adrp(adrp, pc)),
                         self.target + 8)

    def test_repack_add_to_new_page(self):
        pc, adrp, add = self.pair("add")
        new = 0x40000008
        nadrp = repack_adrp(adrp, pc, new & ~0xFFF)
        nadd = repack_sub(add, new, True)
        self.assertEqual(decode_adrp(nadrp, pc), new & ~0xFFF)
        self.assertEqual(decode_add(nadd, decode_adrp(nadrp, pc)), new)

    def test_repack_ldr_to_new_page(self):
        pc, adrp, ldr = self.pair("ldr")
        new = 0x40000010
        nadrp = repack_adrp(adrp, pc, new & ~0xFFF)
        nldr = repack_sub(ldr, new, False)
        self.assertEqual(decode_adrp(nadrp, pc), new & ~0xFFF)
        self.assertEqual(decode_ldr(nldr, decode_adrp(nadrp, pc)), new)


if __name__ == "__main__":
    unittest.main()