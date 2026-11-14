"""Guard the dnsk_core_api layout and the single RW entry point."""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "core" / "include" / "dnsk_core.h"
CORE = ROOT / "core" / "dnsk_core.c"
RW = ROOT / "core" / "dnsk_rw.c"
INTERNAL = ROOT / "core" / "dnsk_internal.h"


class CoreApiTest(unittest.TestCase):
    def test_abi_revision(self):
        self.assertIn("#define DNSK_CORE_ABI 5", HEADER.read_text())

    def test_single_rw_entry(self):
        text = HEADER.read_text()
        self.assertIn("int (*rw)(struct dnsk_rw_req *req);", text)
        self.assertIn("struct dnsk_rw_req {", text)
        self.assertIn("(*patch_pointer)", text)
        for opcode in ["DNSK_RW_READ", "DNSK_RW_WRITE", "DNSK_RW_XCHG",
                       "DNSK_RW_CAS", "DNSK_RW_PROBE"]:
            self.assertIn(opcode, text)
        self.assertIn("#define DNSK_RW_F_LOCKED", text)
        self.assertIn("#define DNSK_RW_F_RO", text)
        for lock in ["DNSK_RW_LOCK_MUTEX", "DNSK_RW_LOCK_SPIN_IRQSAVE",
                     "DNSK_RW_LOCK_RWSEM_READ"]:
            self.assertIn(lock, text)

    def test_table_wires_rw(self):
        self.assertIn(".rw = dnsk_api_rw", CORE.read_text())

    def test_backend_declaration_and_definition(self):
        self.assertIn("int dnsk_rw_op(struct dnsk_rw_req *req);",
                      INTERNAL.read_text())
        self.assertIn("int dnsk_rw_op(struct dnsk_rw_req *req)",
                      RW.read_text())


if __name__ == "__main__":
    unittest.main()