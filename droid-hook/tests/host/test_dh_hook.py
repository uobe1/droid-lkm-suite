#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host selftest for the Droid-Hook trampoline and shadow layers.

Builds src/dh_hook.c, src/dh_shadow.c and dh_hook_harness.c into one shared
library with the host compiler (no kernel headers), then drives every behaviour
through ctypes.  The harness supplies stubs for the teammate-owned dh_insn_* /
dh_txn_* / dh_read entry points, so this test covers only this task's files.
"""

import ctypes
import gc
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INC = ROOT / 'include'
SRC = ROOT / 'src'
HOST = ROOT / 'tests' / 'host'
SO = Path('/tmp/libdh_hook_test.so')

DH_OK = 0
DH_E_INVAL = -22
DH_E_NOSPC = -28
DH_E_BUSY = -16
DH_E_NOENT = -2
DH_E_OPNOTSUPP = -95
DH_E_QUIESCE = -1001
DH_KEY_LEN = 48
DH_ORIGIN_INSNS = 4
DH_JUMP_INSNS = 4


class DhHook(ctypes.Structure):
    """Mirror of struct dh_hook (80 bytes on a 64-bit host)."""
    _fields_ = [('key', ctypes.c_char * DH_KEY_LEN),
                ('target', ctypes.c_void_p),
                ('trampoline', ctypes.c_void_p),
                ('chain', ctypes.c_void_p),
                ('nr_chain', ctypes.c_uint32),
                ('installed', ctypes.c_uint8)]


class HookResult(ctypes.Structure):
    """Mirror of the harness's struct hook_result (int + struct dh_hook)."""
    _fields_ = [('ret', ctypes.c_int), ('hook', DhHook)]


class ShadowResult(ctypes.Structure):
    _fields_ = [('item', ctypes.c_size_t), ('size', ctypes.c_size_t),
                ('id', ctypes.c_uint32), ('obj', ctypes.c_void_p)]


HOOK_PTR = ctypes.POINTER(DhHook)
BEFORE = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
AFTER = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
ORIGIN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
# A callback slot that may legitimately be NULL: ctypes rejects None for a
# CFUNCTYPE argtype, so the NULL contract could never be exercised.  Use a
# plain void pointer for parameters that the NULL test passes None to.
NULLABLE_CB = ctypes.c_void_p

# Keep CFUNCTYPE objects alive for the whole test session; otherwise ctypes
# may collect the thunk while C still holds the pointer.
_CALLBACKS = []


def build():
    subprocess.run(
        ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fPIC',
         '-shared', '-I', str(INC),
         '-o', str(SO),
         str(SRC / 'dh_hook.c'), str(SRC / 'dh_shadow.c'),
         str(HOST / 'dh_hook_harness.c')],
        check=True, capture_output=True, text=True)
    return ctypes.CDLL(str(SO))


class DhHookTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = build()

    def setUp(self):
        lib = self.lib
        lib.harness_reset.argtypes = [ctypes.c_int]
        lib.harness_reset.restype = None
        lib.harness_install.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
        lib.harness_install.restype = HookResult
        lib.harness_uninstall.argtypes = [HOOK_PTR]
        lib.harness_uninstall.restype = ctypes.c_int
        lib.harness_wrap.argtypes = [HOOK_PTR, NULLABLE_CB, NULLABLE_CB]
        lib.harness_wrap.restype = ctypes.c_int
        lib.harness_chain_run.argtypes = [HOOK_PTR, ctypes.c_void_p, ORIGIN]
        lib.harness_chain_run.restype = ctypes.c_int
        lib.harness_text.restype = ctypes.c_void_p
        lib.harness_text_word.argtypes = [ctypes.c_size_t]
        lib.harness_text_word.restype = ctypes.c_uint32
        lib.harness_arbitrate_calls.restype = ctypes.c_int
        lib.harness_arbitrate_addr.restype = ctypes.c_void_p
        lib.harness_registry_count.restype = ctypes.c_int
        lib.harness_live_tramps.restype = ctypes.c_int
        lib.harness_relo_len.argtypes = [HOOK_PTR]
        lib.harness_relo_len.restype = ctypes.c_size_t
        lib.harness_relo_word.argtypes = [HOOK_PTR, ctypes.c_size_t]
        lib.harness_relo_word.restype = ctypes.c_uint32
        lib.harness_jump_word.argtypes = [HOOK_PTR, ctypes.c_size_t]
        lib.harness_jump_word.restype = ctypes.c_uint32
        lib.harness_origin_word.argtypes = [HOOK_PTR, ctypes.c_size_t]
        lib.harness_origin_word.restype = ctypes.c_uint32
        lib.harness_text_writes.restype = ctypes.c_int
        lib.harness_quiesce_calls.restype = ctypes.c_int
        lib.harness_quiesce_depth.restype = ctypes.c_int
        lib.harness_icache_calls.restype = ctypes.c_int
        lib.harness_txn_rollbacks.restype = ctypes.c_int
        lib.harness_fail_text_write.argtypes = [ctypes.c_int]
        lib.harness_fail_text_write.restype = None
        lib.harness_shadow_get.argtypes = [ctypes.c_void_p,
                                           ctypes.c_uint32,
                                           ctypes.c_size_t]
        lib.harness_shadow_get.restype = ShadowResult
        lib.harness_shadow_free.argtypes = [ctypes.c_void_p,
                                            ctypes.c_uint32]
        lib.harness_shadow_free.restype = ctypes.c_int
        lib.harness_shadow_put.argtypes = [ctypes.c_size_t]
        lib.harness_shadow_put.restype = ctypes.c_int
        lib.harness_shadow_generation.restype = ctypes.c_uint32
        lib.harness_shadow_live.restype = ctypes.c_int
        lib.harness_shadow_fill.argtypes = [ctypes.c_size_t, ctypes.c_ubyte,
                                           ctypes.c_size_t]
        lib.harness_shadow_fill.restype = None
        lib.harness_shadow_dirty.argtypes = [ctypes.c_size_t, ctypes.c_size_t]
        lib.harness_shadow_dirty.restype = ctypes.c_int
        lib.harness_hook_size.restype = ctypes.c_size_t
        lib.harness_hook_result_size.restype = ctypes.c_size_t
        lib.harness_shadow_item_size.restype = ctypes.c_size_t
        lib.harness_alloc_count.restype = ctypes.c_int
        lib.harness_free_count.restype = ctypes.c_int
        lib.harness_free_order.argtypes = [ctypes.c_int]
        lib.harness_free_order.restype = ctypes.c_int
        lib.harness_free_quiesce.argtypes = [ctypes.c_int]
        lib.harness_free_quiesce.restype = ctypes.c_int
        lib.harness_free_text_writes.argtypes = [ctypes.c_int]
        lib.harness_free_text_writes.restype = ctypes.c_int
        self.lib.harness_reset(0)

    def install(self, key=b'hook-a'):
        # Keep the owning struct alive on the instance: a pointer into a
        # temporary ctypes object would dangle as soon as install() returns.
        self._last_result = self.lib.harness_install(
            key, ctypes.c_void_p(0xDEADBEEF))
        return (self._last_result,
                ctypes.pointer(self._last_result.hook))

    # ---- ctypes boundary: layout and object lifetime ----

    def test_struct_layout_matches_python_mirror(self):
        """A short mirror would truncate the by-value return.

        harness_install() returns struct hook_result by value.  If the Python
        mirror were smaller than the C struct, ctypes would materialise only
        the prefix and every later field read through pointer(r.hook) would be
        an out-of-bounds read of the copy.  Asserting the size and every field
        offset from C is the only way to rule that out.
        """
        self.assertEqual(ctypes.sizeof(DhHook), self.lib.harness_hook_size())
        self.assertEqual(ctypes.sizeof(HookResult),
                         self.lib.harness_hook_result_size())
        self.assertEqual(ctypes.sizeof(ShadowResult), 4 * 8)
        self.assertEqual(DhHook.key.offset, 0)
        self.assertEqual(DhHook.target.offset, DH_KEY_LEN)
        self.assertEqual(DhHook.trampoline.offset, DH_KEY_LEN + 8)
        self.assertEqual(DhHook.chain.offset, DH_KEY_LEN + 16)
        self.assertEqual(DhHook.nr_chain.offset, DH_KEY_LEN + 24)
        self.assertEqual(DhHook.installed.offset, DH_KEY_LEN + 28)
        self.assertEqual(HookResult.hook.offset, 8,
                         'hook must follow the int plus its padding')
        self.assertEqual(ShadowResult.item.offset, 0)
        self.assertEqual(ShadowResult.size.offset, 8)
        self.assertEqual(ShadowResult.id.offset, 16)
        self.assertEqual(ShadowResult.obj.offset, 24)
        # struct dh_shadow_item is obj+id+size plus a zero-length flexible
        # array, so the C size is 24 on a 64-bit host.
        self.assertEqual(self.lib.harness_shadow_item_size(), 24)

    def test_returned_struct_pointer_outlives_the_temporary(self):
        """The pointer must own its target, not borrow an instance attribute.

        install() keeps the result on self so a temporary cannot be collected,
        but that is belt-and-braces: ctypes.pointer() keeps a reference to the
        object it points into.  Dropping every other name and forcing a
        collection proves the pointer is self-sufficient instead of relying on
        the attribute staying alive.
        """
        r, h = self.install()
        self.assertEqual(r.ret, DH_OK)
        addr = ctypes.addressof(h.contents)
        self.assertEqual(h.contents.key, b'hook-a')
        self.assertEqual(self.lib.harness_relo_len(h),
                         DH_ORIGIN_INSNS + DH_JUMP_INSNS)

        del r
        del self._last_result
        gc.collect()

        self.assertTrue(h._objects,
                        'a ctypes pointer must anchor its referent')
        self.assertEqual(ctypes.addressof(h.contents), addr,
                         'pointer target must not move')
        self.assertEqual(h.contents.key, b'hook-a',
                         'struct content must survive collection')
        self.assertEqual(h.contents.target, self.lib.harness_text(),
                         'C writes through the pointer, so the copy must be live')
        self.assertEqual(self.lib.harness_relo_len(h),
                         DH_ORIGIN_INSNS + DH_JUMP_INSNS,
                         'trampoline pointer inside the struct must stay valid')
        self.assertEqual(self.lib.harness_uninstall(h), DH_OK,
                         'a pointer that outlived the temporary must still work')

    # ---- install / registry ----

    def test_install_happy_path_and_trampoline_shape(self):
        r, h = self.install()
        self.assertEqual(r.ret, DH_OK, 'install must succeed')
        # Four displaced instructions are backed up verbatim.
        for i in range(DH_ORIGIN_INSNS):
            self.assertEqual(self.lib.harness_origin_word(h, i),
                             0x1000 + i)
        # The entry jump is written into the fake text area.
        self.assertEqual(self.lib.harness_text_writes(), 1)
        self.assertEqual(self.lib.harness_text_word(0), 0xD4000000)
        # relo = 4 displaced + 4 back-jump words.
        self.assertEqual(self.lib.harness_relo_len(h),
                         DH_ORIGIN_INSNS + DH_JUMP_INSNS)
        for i in range(DH_ORIGIN_INSNS):
            self.assertEqual(self.lib.harness_relo_word(h, i),
                             (0x1000 + i) ^ 0xA5A5A5A5)
        self.assertEqual(self.lib.harness_registry_count(), 1)
        self.assertEqual(self.lib.harness_live_tramps(), 1)
        self.assertGreaterEqual(self.lib.harness_icache_calls(), 1)

    def test_duplicate_key_returns_busy(self):
        r1, _ = self.install(b'dup')
        self.assertEqual(r1.ret, DH_OK)
        r2, _ = self.install(b'dup')
        self.assertEqual(r2.ret, DH_E_BUSY, 'duplicate key must be DH_E_BUSY')
        self.assertEqual(self.lib.harness_registry_count(), 1)

    def test_duplicate_target_returns_busy(self):
        r1, _ = self.install(b'a')
        self.assertEqual(r1.ret, DH_OK)
        r2, _ = self.install(b'b')
        self.assertEqual(r2.ret, DH_E_BUSY, 'second writer at same address')

    def test_arbitrate_is_called_with_target(self):
        r, _ = self.install()
        self.assertEqual(r.ret, DH_OK)
        self.assertEqual(self.lib.harness_arbitrate_calls(), 1)
        self.assertEqual(self.lib.harness_arbitrate_addr(),
                         self.lib.harness_text())

    def test_arbitrate_rejection_prevents_install(self):
        self.lib.harness_reset(1)
        r, _ = self.install()
        self.assertEqual(r.ret, DH_E_BUSY, 'rejected arbitration => DH_E_BUSY')
        self.assertEqual(self.lib.harness_arbitrate_calls(), 1)
        self.assertEqual(self.lib.harness_registry_count(), 0,
                         'no registry entry may be left behind')
        self.assertEqual(self.lib.harness_text_writes(), 0,
                         'no text write may happen')
        self.assertEqual(self.lib.harness_live_tramps(), 0)

    def test_install_rollback_on_text_write_failure(self):
        self.lib.harness_fail_text_write(1)
        r, _ = self.install()
        self.assertNotEqual(r.ret, DH_OK)
        self.assertGreaterEqual(self.lib.harness_txn_rollbacks(), 1,
                                'failure must roll the transaction back')
        self.assertEqual(self.lib.harness_registry_count(), 0)
        self.assertEqual(self.lib.harness_live_tramps(), 0,
                         'trampoline must be released on failure')
        self.assertEqual(self.lib.harness_text_word(0), 0x1000,
                         'original bytes must be restored')

    # ---- chain ----

    def test_before_order_after_reverse_and_skip_origin(self):
        r, h = self.install()
        self.assertEqual(r.ret, DH_OK)
        log = []

        def mk(label):
            def cb(fargs):
                log.append(label)
                return 0
            return BEFORE(cb)

        before1, before2 = mk('b1'), mk('b2')
        after1, after2 = BEFORE(lambda _a: (log.append('a1'), 0)[1]), \
                         BEFORE(lambda _a: (log.append('a2'), 0)[1])
        _CALLBACKS.extend([before1, before2, after1, after2])
        self.assertEqual(self.lib.harness_wrap(h, before1, after1), DH_OK)
        self.assertEqual(self.lib.harness_wrap(h, before2, after2), DH_OK)

        origin = ORIGIN(lambda _a: (log.append('origin'), 7)[1])
        _CALLBACKS.append(origin)
        ret = self.lib.harness_chain_run(h, None, origin)
        self.assertEqual(ret, 7)
        self.assertEqual(log, ['b1', 'b2', 'origin', 'a2', 'a1'],
                         'before in order, after in reverse')

    def test_skip_origin_when_before_returns_nonzero(self):
        r, h = self.install()
        self.assertEqual(r.ret, DH_OK)
        log = []
        skip = BEFORE(lambda _a: (log.append('skip'), 1)[1])
        after = BEFORE(lambda _a: (log.append('after'), 0)[1])
        origin = ORIGIN(lambda _a: (log.append('origin'), 0)[1])
        _CALLBACKS.extend([skip, after, origin])
        self.assertEqual(self.lib.harness_wrap(h, skip, after), DH_OK)
        ret = self.lib.harness_chain_run(h, None, origin)
        self.assertEqual(ret, 1, 'skip result is propagated')
        self.assertEqual(log, ['skip', 'after'],
                         'origin must not run when a before skips it')

    def test_wrap_bounded_and_invalid(self):
        r, h = self.install()
        self.assertEqual(r.ret, DH_OK)
        self.assertEqual(self.lib.harness_wrap(h, None, None), DH_E_INVAL)
        cb = BEFORE(lambda _a: 0)
        _CALLBACKS.append(cb)
        for _i in range(16):
            self.assertEqual(self.lib.harness_wrap(h, cb, cb), DH_OK)
        self.assertEqual(self.lib.harness_wrap(h, cb, cb), DH_E_NOSPC,
                         'chain must be bounded')

    # ---- uninstall ----

    def test_uninstall_restores_original_and_quiesces(self):
        r, h = self.install()
        self.assertEqual(r.ret, DH_OK)
        ret = self.lib.harness_uninstall(h)
        self.assertEqual(ret, DH_OK)
        self.assertEqual(self.lib.harness_text_word(0), 0x1000,
                         'original instruction restored')
        self.assertEqual(self.lib.harness_quiesce_calls(), 1,
                         'one quiesce window')
        self.assertEqual(self.lib.harness_quiesce_depth(), 0,
                         'quiesce must be balanced')
        self.assertEqual(self.lib.harness_registry_count(), 0)
        self.assertEqual(self.lib.harness_live_tramps(), 0,
                         'trampoline released')
        self.assertEqual(h.contents.installed, 0,
                         'installed flag must be cleared')

    def test_uninstall_releases_in_reverse_order_inside_quiesce(self):
        """Reverse-of-install is observable through the backend, not guessed.

        install allocates the trampoline first and the chain second.  A correct
        uninstall restores the original bytes, then releases the chain (alloc
        index 1) before the trampoline (index 0), and does all of it between
        quiesce_begin() and quiesce_end().  Return codes cannot show any of
        that, so the harness records the trace.
        """
        r, h = self.install()
        self.assertEqual(r.ret, DH_OK)
        self.assertEqual(self.lib.harness_alloc_count(), 2,
                         'trampoline then chain')
        self.assertEqual(self.lib.harness_free_count(), 0)

        self.assertEqual(self.lib.harness_uninstall(h), DH_OK)

        self.assertEqual(self.lib.harness_free_count(), 2,
                         'both allocations are released exactly once')
        self.assertEqual(self.lib.harness_free_order(0), 1,
                         'chain (second allocation) released first')
        self.assertEqual(self.lib.harness_free_order(1), 0,
                         'trampoline (first allocation) released last')
        for i in (0, 1):
            self.assertEqual(self.lib.harness_free_quiesce(i), 1,
                             'release must happen inside the quiesce window')
            self.assertGreaterEqual(
                self.lib.harness_free_text_writes(i), 1,
                'original bytes must be restored before anything is freed')

    def test_uninstall_quiesce_failure_keeps_hook_installed(self):
        r, h = self.install()
        self.assertEqual(r.ret, DH_OK)
        self.lib.harness_fail_text_write(1)
        ret = self.lib.harness_uninstall(h)
        self.assertEqual(ret, DH_E_QUIESCE,
                         'failed quiesce must be reported, never forced')
        self.assertEqual(self.lib.harness_registry_count(), 1,
                         'registry entry must survive a failed unload')
        self.assertEqual(self.lib.harness_live_tramps(), 1,
                         'trampoline must survive a failed unload')
        self.assertEqual(self.lib.harness_quiesce_depth(), 0,
                         'quiesce still balanced on the error path')

    # ---- shadow ----

    def test_shadow_get_or_alloc_is_idempotent(self):
        obj = ctypes.c_void_p(0x1234)
        a = self.lib.harness_shadow_get(obj, 7, 32)
        b = self.lib.harness_shadow_get(obj, 7, 32)
        self.assertNotEqual(a.item, 0)
        self.assertEqual(a.item, b.item, 'same (obj,id) => same pointer')
        self.assertEqual(a.size, b.size)
        self.assertEqual(a.size, 32)
        self.assertEqual(a.id, 7)
        self.assertEqual(a.obj, 0x1234)
        self.assertEqual(self.lib.harness_shadow_live(), 1)

    def test_shadow_different_key_is_different_item(self):
        obj = ctypes.c_void_p(0x1234)
        a = self.lib.harness_shadow_get(obj, 7, 32)
        b = self.lib.harness_shadow_get(obj, 8, 32)
        self.assertNotEqual(a.item, b.item, 'different id => different item')
        self.assertEqual(self.lib.harness_shadow_live(), 2)

    def test_shadow_size_mismatch_rejected(self):
        obj = ctypes.c_void_p(0x1234)
        a = self.lib.harness_shadow_get(obj, 7, 32)
        self.assertNotEqual(a.item, 0)
        b = self.lib.harness_shadow_get(obj, 7, 64)
        self.assertEqual(b.item, 0, 'size mismatch must not reallocate')

    def test_free_then_get_returns_fresh_object(self):
        obj = ctypes.c_void_p(0x1234)
        a = self.lib.harness_shadow_get(obj, 7, 32)
        self.assertEqual(a.item != 0, True)
        # Dirty the payload so a stale entry would be observable.
        self.lib.harness_shadow_fill(a.item, 0xA5, 32)
        self.assertEqual(self.lib.harness_shadow_dirty(a.item, 32), 1)

        self.assertEqual(self.lib.harness_shadow_free(obj, 7), DH_OK)

        b = self.lib.harness_shadow_get(obj, 7, 32)
        self.assertNotEqual(b.item, 0, 'get must succeed after free')
        # The address may legitimately be recycled from the pool; what must
        # hold is that the key was detached and the new item starts clean.
        self.assertEqual(self.lib.harness_shadow_dirty(b.item, 32), 0,
                         'a detached key must not hand back the old payload')
        self.assertEqual(b.size, 32)
        self.assertEqual(b.obj, 0x1234)
        self.assertEqual(b.id, 7)

        self.assertEqual(self.lib.harness_shadow_free(obj, 7), DH_OK)
        self.assertEqual(self.lib.harness_shadow_free(obj, 7), DH_E_NOENT)

    def test_delayed_reclaim_until_last_put(self):
        obj = ctypes.c_void_p(0x1234)
        a = self.lib.harness_shadow_get(obj, 7, 32)
        self.lib.harness_shadow_get(obj, 7, 32)  # refs = 2
        self.assertEqual(self.lib.harness_shadow_free(obj, 7), DH_OK)
        self.assertEqual(self.lib.harness_shadow_live(), 1,
                         'detached item stays alive with an outstanding ref')
        self.assertEqual(self.lib.harness_shadow_put(a.item), DH_OK)
        self.assertEqual(self.lib.harness_shadow_put(a.item), DH_OK)
        self.assertEqual(self.lib.harness_shadow_live(), 0,
                         'storage reclaimed only after the last put')


if __name__ == '__main__':
    unittest.main()
