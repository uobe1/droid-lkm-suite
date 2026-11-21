#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host selftest for the Droid-Hook RW wrapper and transaction rollback layer.

Drives the real dh_* implementations through a pure-host mock backend
(dh_rw_harness.c).  The harness is compiled into a shared library with the
same strict flags used for the library sources, so a compile failure is a
real contract violation, not a Python-side shim.
"""

import ctypes
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
INC = ROOT / 'include'
SRC = ROOT / 'src'
SO = Path('/tmp/libdh_rw_test.so')

CC = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
      '-I', str(INC), '-shared', '-fPIC',
      str(SRC / 'dh_rw.c'), str(SRC / 'dh_txn.c'), str(HERE / 'dh_rw_harness.c'),
      '-o', str(SO)]

# DH_* errnos from dh.h (negative errnos, plain ints).
DH_OK = 0
DH_E_INVAL = -22
DH_E_NOSPC = -28
DH_E_BUSY = -16
DH_E_VERIFY = -1000
DH_ROUTE_ALIAS = 1


class DhRwTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(CC, check=True, capture_output=True, text=True)
        cls.lib = ctypes.CDLL(str(SO))
        cls._bind()

    @classmethod
    def _bind(cls):
        L = cls.lib
        L.dh_harness_reset.restype = None
        L.dh_harness_backend.restype = ctypes.c_void_p
        L.dh_harness_mem.restype = ctypes.c_void_p
        L.dh_harness_mem_size.restype = ctypes.c_ulong
        L.dh_harness_init.restype = ctypes.c_int
        L.dh_harness_fini.restype = ctypes.c_int
        L.dh_harness_write.argtypes = [ctypes.c_ulong, ctypes.c_void_p,
                                       ctypes.c_size_t]
        L.dh_harness_write.restype = ctypes.c_int
        L.dh_harness_read.argtypes = [ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_size_t]
        L.dh_harness_read.restype = ctypes.c_int
        L.dh_harness_xchg.argtypes = [ctypes.c_ulong, ctypes.c_uint64,
                                      ctypes.POINTER(ctypes.c_uint64)]
        L.dh_harness_xchg.restype = ctypes.c_int
        L.dh_harness_cas.argtypes = [ctypes.c_ulong, ctypes.c_uint64,
                                     ctypes.c_uint64]
        L.dh_harness_cas.restype = ctypes.c_int
        L.dh_harness_probe.argtypes = [ctypes.c_char_p]
        L.dh_harness_probe.restype = ctypes.c_void_p
        L.dh_harness_txn_begin.restype = ctypes.c_int
        L.dh_harness_txn_record.argtypes = [ctypes.c_ulong, ctypes.c_size_t,
                                            ctypes.c_int]
        L.dh_harness_txn_record.restype = ctypes.c_int
        L.dh_harness_txn_commit.restype = ctypes.c_int
        L.dh_harness_txn_rollback.restype = ctypes.c_int
        L.dh_harness_txn_n.restype = ctypes.c_uint32
        L.dh_harness_txn_active.restype = ctypes.c_int
        L.dh_harness_txn_failed.restype = ctypes.c_int
        L.dh_harness_quiesce_calls.restype = ctypes.c_ulong
        L.dh_harness_quiesce_depth.restype = ctypes.c_ulong
        L.dh_harness_ro_write_calls.restype = ctypes.c_ulong
        L.dh_harness_corrupt_next_write.restype = None
        L.dh_harness_fail_rw.argtypes = [ctypes.c_int]
        L.dh_harness_fail_rw.restype = None
        L.dh_harness_fail_ro_write.argtypes = [ctypes.c_int]
        L.dh_harness_fail_ro_write.restype = None
        L.dh_harness_read_byte.argtypes = [ctypes.c_ulong]
        L.dh_harness_read_byte.restype = ctypes.c_ubyte
        L.dh_harness_write_byte.argtypes = [ctypes.c_ulong, ctypes.c_ubyte]
        L.dh_harness_write_byte.restype = None
        L.dh_harness_init_with_null_rw.restype = ctypes.c_int

    def setUp(self):
        self.lib.dh_harness_reset()
        self.assertEqual(self.lib.dh_harness_init(), DH_OK)

    def tearDown(self):
        # fini() is exercised explicitly in some tests; a second call on an
        # already-finalised ctx must fail closed, not crash.
        self.assertIn(self.lib.dh_harness_fini(), (DH_OK, DH_E_INVAL))

    # ---- helpers ----

    def _write(self, off, data):
        buf = (ctypes.c_ubyte * len(data)).from_buffer_copy(bytes(data))
        return self.lib.dh_harness_write(off, buf, len(data))

    def _read(self, off, n):
        buf = (ctypes.c_ubyte * n)()
        ret = self.lib.dh_harness_read(off, buf, n)
        return ret, bytes(buf)

    # ---- tests ----

    def test_01_normal_read_write(self):
        self.assertEqual(self._write(16, b'\x01\x02\x03\x04'), DH_OK)
        ret, got = self._read(16, 4)
        self.assertEqual(ret, DH_OK)
        self.assertEqual(got, b'\x01\x02\x03\x04')

    def test_02_xchg_returns_old_and_stores_new(self):
        old = ctypes.c_uint64(0)
        self.assertEqual(self._write(64, (0x1122334455667788).to_bytes(8, 'little')),
                         DH_OK)
        self.assertEqual(self.lib.dh_harness_xchg(64, 0xAABBCCDDEEFF0011,
                                                  ctypes.byref(old)), DH_OK)
        self.assertEqual(old.value, 0x1122334455667788)
        ret, got = self._read(64, 8)
        self.assertEqual(ret, DH_OK)
        self.assertEqual(int.from_bytes(got, 'little'), 0xAABBCCDDEEFF0011)

    def test_03_cas_success_and_mismatch(self):
        self.assertEqual(self._write(72, (0x10).to_bytes(8, 'little')), DH_OK)
        self.assertEqual(self.lib.dh_harness_cas(72, 0x10, 0x20), DH_OK)
        ret, got = self._read(72, 8)
        self.assertEqual(ret, DH_OK)
        self.assertEqual(int.from_bytes(got, 'little'), 0x20)
        self.assertEqual(self.lib.dh_harness_cas(72, 0x10, 0x30), DH_E_BUSY)
        ret, got = self._read(72, 8)
        self.assertEqual(int.from_bytes(got, 'little'), 0x20)

    def test_04_probe_and_null_backend_rejection(self):
        addr = self.lib.dh_harness_probe(b'cgroup_subsys')
        self.assertNotIn(addr, (None, 0))
        # ctypes maps a NULL c_void_p return to Python None.
        self.assertIn(self.lib.dh_harness_probe(b'no_such_symbol'), (None, 0))
        self.assertEqual(self.lib.dh_harness_init_with_null_rw(), DH_E_INVAL)

    def test_05_rollback_restores_in_reverse_order(self):
        # Three recorded slots, then three writes.  Rollback must replay the
        # undo entries from the newest to the oldest.
        base = [128, 136, 144]
        original = {off: bytes([0xA0 + i, 0xB0 + i, 0xC0 + i])
                    for i, off in enumerate(base)}
        for off, val in original.items():
            self.assertEqual(self._write(off, val), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_begin(), DH_OK)
        for off in base:
            self.assertEqual(self.lib.dh_harness_txn_record(off, 3,
                                                            DH_ROUTE_ALIAS), DH_OK)
        for off in base:
            self.assertEqual(self._write(off, b'\x01\x02\x03'), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_n(), 3)

        before = self.lib.dh_harness_quiesce_calls()
        self.assertEqual(self.lib.dh_harness_txn_rollback(), DH_OK)
        self.assertEqual(self.lib.dh_harness_quiesce_calls(), before + 1)
        self.assertEqual(self.lib.dh_harness_quiesce_depth(), 0)
        self.assertEqual(self.lib.dh_harness_ro_write_calls(), 3)
        for off, val in original.items():
            ret, got = self._read(off, 3)
            self.assertEqual(ret, DH_OK)
            self.assertEqual(got, val)
        self.assertEqual(self.lib.dh_harness_txn_active(), 0)

    def test_05b_rollback_order_is_observable(self):
        # Two undo entries target the SAME address.  If rollback replayed in
        # forward order, the older snapshot would win and the value would be
        # wrong.  Only reverse replay restores the original byte.
        off = 200
        self.assertEqual(self._write(off, b'\xE1'), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_begin(), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_record(off, 1,
                                                        DH_ROUTE_ALIAS), DH_OK)
        self.assertEqual(self._write(off, b'\xE2'), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_record(off, 1,
                                                        DH_ROUTE_ALIAS), DH_OK)
        self.assertEqual(self._write(off, b'\xE3'), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_rollback(), DH_OK)
        self.assertEqual(self.lib.dh_harness_read_byte(off), 0xE1)

    def test_06_undo_full_returns_nospace(self):
        self.assertEqual(self.lib.dh_harness_txn_begin(), DH_OK)
        for i in range(8):
            self.assertEqual(self.lib.dh_harness_txn_record(256 + i * 8, 8,
                                                            DH_ROUTE_ALIAS), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_n(), 8)
        self.assertEqual(self.lib.dh_harness_txn_record(256 + 64, 8,
                                                        DH_ROUTE_ALIAS),
                         DH_E_NOSPC)
        self.assertEqual(self.lib.dh_harness_txn_n(), 8)
        self.assertEqual(self.lib.dh_harness_txn_rollback(), DH_OK)

    def test_07_readback_verify_marks_failed_and_commit_rolls_back(self):
        self.assertEqual(self._write(400, b'\xAA\xBB'), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_begin(), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_record(400, 2,
                                                        DH_ROUTE_ALIAS), DH_OK)
        self.lib.dh_harness_corrupt_next_write()
        self.assertEqual(self._write(400, b'\xCC\xDD'), DH_E_VERIFY)
        self.assertEqual(self.lib.dh_harness_txn_failed(), 1)
        # A failed transaction must not commit; commit() rolls it back.
        self.assertEqual(self.lib.dh_harness_txn_commit(), DH_E_VERIFY)
        self.assertEqual(self.lib.dh_harness_txn_active(), 0)
        ret, got = self._read(400, 2)
        self.assertEqual(ret, DH_OK)
        self.assertEqual(got, b'\xAA\xBB')

    def test_08_ctx_fini_auto_rolls_back_open_transaction(self):
        self.assertEqual(self._write(512, b'\x11\x22'), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_begin(), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_record(512, 2,
                                                        DH_ROUTE_ALIAS), DH_OK)
        self.assertEqual(self._write(512, b'\x33\x44'), DH_OK)
        before = self.lib.dh_harness_quiesce_calls()
        self.assertEqual(self.lib.dh_harness_fini(), DH_OK)
        self.assertEqual(self.lib.dh_harness_quiesce_calls(), before + 1)
        self.assertEqual(self.lib.dh_harness_quiesce_depth(), 0)
        self.assertEqual(self.lib.dh_harness_txn_active(), 0)
        self.assertEqual(self.lib.dh_harness_read_byte(512), 0x11)
        self.assertEqual(self.lib.dh_harness_read_byte(513), 0x22)
        # fini() twice must fail closed.
        self.assertEqual(self.lib.dh_harness_fini(), DH_E_INVAL)

    def test_09_quiesce_window_is_balanced_on_rollback(self):
        self.assertEqual(self.lib.dh_harness_txn_begin(), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_record(600, 4,
                                                        DH_ROUTE_ALIAS), DH_OK)
        self.assertEqual(self.lib.dh_harness_txn_rollback(), DH_OK)
        self.assertEqual(self.lib.dh_harness_quiesce_depth(), 0)
        self.assertGreaterEqual(self.lib.dh_harness_quiesce_calls(), 1)

    def test_10_large_transfer_is_chunked_and_still_verified(self):
        # 600 bytes spans the 256-byte chunk cap; must succeed and verify.
        data = bytes((i * 7 + 3) & 0xFF for i in range(600))
        self.assertEqual(self._write(1024, data), DH_OK)
        ret, got = self._read(1024, 600)
        self.assertEqual(ret, DH_OK)
        self.assertEqual(got, data)

    def test_11_unaligned_xchg_is_rejected(self):
        self.assertEqual(self.lib.dh_harness_xchg(1025, 1, None), DH_E_INVAL)


if __name__ == '__main__':
    unittest.main()
