#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host selftest for the core RW ABI 5 -> struct dh_backend adapter.

Compiles droid-hook/runtime/dh_core_backend.c together with a mock-core
harness (no kernel headers), then drives every scenario and asserts the
adapter's behaviour: field conversion, data-path routing, F_TEXT/F_RO routing,
mandatory-primitive validation, >256B / cross-page splitting, failure recovery
and symbol resolution.

Chunk sequences are DERIVED, never captured.  The adapter may only emit
page-local chunks of at most DNSK_RW_MAX bytes starting at a given address, so
the expected split follows from (page, chunk max, base alignment mod page,
start offset, total length) - all of which the harness prints itself.  An
earlier revision hardcoded one observed split and broke on CI because the mock
buffer's page offset is a linker decision (0x60 locally, 0x40 on CI): the
sequence shifted by exactly that delta while the total stayed correct.

This is a host-only gate: it never builds a kernel module and never needs a
kernel tree.

Environment overrides exist only for local layout experiments:
  DH_RUNTIME_BIN     output binary path (default /tmp/dh_runtime_harness_test)
  DH_RUNTIME_CFLAGS  extra compiler flags, e.g. "-O0" or "-DDH_MOCK_SHIFT=64"
CI uses the defaults.
"""

import os
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'runtime'
INC = ROOT / 'include'
HARNESS_SRC = ROOT / 'tests' / 'host' / 'dh_runtime_harness.c'
BACKEND_SRC = RUNTIME / 'dh_core_backend.c'
BIN = Path(os.environ.get('DH_RUNTIME_BIN', '/tmp/dh_runtime_harness_test'))
EXTRA_CFLAGS = os.environ.get('DH_RUNTIME_CFLAGS', '').split()

COMPILE_CMD = [
    'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *EXTRA_CFLAGS,
    '-I', str(INC), '-I', str(RUNTIME),
    str(HARNESS_SRC), str(BACKEND_SRC), '-o', str(BIN),
]

# The default split scenario: 600 bytes, deliberately larger than one page so
# the planner must split, and not a multiple of the chunk max so the tail is
# short.
SPLIT_LEN = 600
SPLIT_START = 8


def compile_harness():
    return subprocess.run(COMPILE_CMD, capture_output=True, text=True)


def run(*args):
    r = subprocess.run([str(BIN), *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError(f'harness {args} exited {r.returncode}: {r.stderr}')
    return parse(r.stdout)


def parse(out):
    """Turn KEY=VALUE tokens and CALL lines into a dict + call list."""
    d, calls = {}, []
    for line in out.splitlines():
        if line.startswith('CALL '):
            m = re.match(r'CALL (\d+) opcode=(\d+) flags=(\d+) len=(\d+) '
                         r'off=(\d+) ret=(-?\d+)', line)
            if m:
                calls.append({
                    'idx': int(m.group(1)), 'opcode': int(m.group(2)),
                    'flags': int(m.group(3)), 'len': int(m.group(4)),
                    'off': int(m.group(5)), 'ret': int(m.group(6)),
                })
            continue
        # A line may carry several KEY=VALUE tokens (e.g. "RC=0 SLOT=0x1").
        # The leading bare word (BACKEND) is also a key whose value is the
        # remainder of the line, so assertions can match either form.
        tokens = line.split()
        if tokens and '=' not in tokens[0] and len(tokens) > 1:
            d[tokens[0]] = line[len(tokens[0]):].strip()
            tokens = tokens[1:]
        for tok in tokens:
            if '=' in tok:
                k, v = tok.split('=', 1)
                d[k] = v
    d['calls'] = calls
    return d


def plan_chunks(base_align, start_off, total, page, chunk_max):
    """Derive the only legal chunk sequence for this request.

    Invariants the adapter must satisfy, expressed so a different page size,
    chunk max or buffer alignment changes the expectation instead of breaking
    the test:

      * no chunk crosses a page boundary
      * no chunk exceeds chunk_max
      * the chunks exactly cover [start_off, start_off + total)

    The planner is greedy: take as much as the current page allows, capped by
    chunk_max and the bytes remaining.
    """
    if page <= 0 or page & (page - 1):
        raise AssertionError(f'page size {page} is not a power of two; '
                             'the adapter masks with page-1, so the derivation '
                             'would not match')
    if chunk_max <= 0:
        raise AssertionError(f'invalid chunk max {chunk_max}')
    chunks, done = [], 0
    while done < total:
        page_off = (base_align + start_off + done) % page
        take = min(total - done, page - page_off, chunk_max)
        if take <= 0:
            raise AssertionError('planner made no progress')
        chunks.append(take)
        done += take
    return chunks


class DhRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        r = compile_harness()
        if r.returncode != 0:
            raise AssertionError(f'compile failed:\n{r.stdout}\n{r.stderr}')
        cls.compile_stderr = r.stderr

    # ---- helpers ---------------------------------------------------------

    def geometry(self, d):
        """(page, chunk_max, base_align) as reported by the harness."""
        return int(d['PAGE']), int(d['CHUNK_MAX']), int(d['BASE_ALIGN'])

    def expected_split(self, d, start_off, total):
        page, chunk_max, base = self.geometry(d)
        return plan_chunks(base, start_off, total, page, chunk_max)

    def chunk_spans(self, d, opcode):
        """[(off, len)] for every call with this opcode, in call order."""
        return [(c['off'], c['len']) for c in d['calls'] if c['opcode'] == opcode]

    def start_for_page_off(self, d, want_off):
        """Start offset (relative to mock_mem) that lands on page offset
        ``want_off``.  Tests state the absolute page offset they mean, so the
        case keeps its meaning when the harness is built at another base
        alignment."""
        page, _, base = self.geometry(d)
        return (want_off - base) % page

    def assert_split(self, d, start_off, total, opcode):
        """Assert the real chunk sequence equals the derived one, plus the
        three invariants stated directly (so a wrong derivation cannot hide a
        wrong split)."""
        page, chunk_max, base = self.geometry(d)
        expected = plan_chunks(base, start_off, total, page, chunk_max)
        got = self.chunk_spans(d, opcode)

        self.assertEqual([n for _, n in got], expected,
                         f'chunk lengths differ from the derived plan '
                         f'(page={page} chunk_max={chunk_max} '
                         f'base_align={base} start={start_off} len={total})')

        self.assertEqual(len(got), len(expected), 'chunk count differs')
        self.assertEqual(sum(n for _, n in got), total,
                         'chunk lengths must sum to the request length')
        self.assertTrue(all(n <= chunk_max for _, n in got),
                        'a chunk exceeds the core limit')
        for off, n in got:
            self.assertLessEqual((base + off) % page + n, page,
                                 f'chunk at off={off} len={n} crosses a page')

        off = start_off
        for real_off, n in got:
            self.assertEqual(real_off, off, 'chunks must be contiguous')
            off += n
        self.assertEqual(off, start_off + total, 'chunks must cover the range')
        return expected

    def assert_memory_pattern(self, d, start_off, total, written):
        """Bytes in [start, start+total) hold big[i] = i & 0xff after a full
        write, or 0xA5 (the reset pattern) when nothing was written."""
        if written:
            self.assertEqual(int(d['MEM_FIRST']), 0)
            self.assertEqual(int(d['MEM_LAST']), (total - 1) & 0xff)
            self.assertEqual(int(d['MEM_SUM']),
                             sum(i & 0xff for i in range(total)))
        else:
            self.assertEqual(int(d['MEM_FIRST']), 0xA5)
            self.assertEqual(int(d['MEM_LAST']), 0xA5)
            self.assertEqual(int(d['MEM_SUM']), 0xA5 * total)

    # ---- compile / init --------------------------------------------------

    def test_compile_clean(self):
        self.assertEqual(self.compile_stderr.strip(), '')

    def test_init_fills_mandatory_primitives(self):
        d = run('init')
        self.assertEqual(d['INIT'], '0')
        self.assertEqual(d['BACKEND'].split()[0], 'name=dnsk_core_rw5')
        self.assertIn('abi=1', d['BACKEND'])
        for field in ('rw=', 'text=', 'ro=', 'probe=', 'pp='):
            self.assertNotIn(f'{field}(nil)', d['BACKEND'])
        # PAGE defaults to DNSK_RW_MAX and CHUNK_MAX is that same frozen core
        # constant.  Both are ABI values, not layout: asserting them here keeps
        # a silent change of the core limit from passing unnoticed.
        self.assertEqual(d['PAGE'], '256')
        self.assertEqual(d['CHUNK_MAX'], '256')

    def test_bad_api_null(self):
        d = run('bad', 'null')
        self.assertEqual(d['INIT'], '-22')  # DH_E_INVAL
        self.assertIn('name=(null)', d['BACKEND'])

    def test_bad_api_magic(self):
        self.assertEqual(run('bad', 'magic')['INIT'], '-22')

    def test_bad_api_abi(self):
        self.assertEqual(run('bad', 'abi')['INIT'], '-22')

    def test_bad_api_missing_rw(self):
        self.assertEqual(run('bad', 'norw')['INIT'], '-22')

    # ---- data path -------------------------------------------------------

    def test_data_read_one_core_call_no_flags(self):
        d = run('data', 'read')
        self.assertEqual(d['RC'], '0')
        self.assertEqual(len(d['calls']), 1)
        c = d['calls'][0]
        self.assertEqual((c['opcode'], c['flags'], c['len'], c['off']),
                         (1, 0, 8, 16))  # DNSK_RW_READ, no F_TEXT/F_RO

    def test_data_write_one_core_call_no_flags(self):
        d = run('data', 'write')
        self.assertEqual(d['RC'], '0')
        self.assertEqual(len(d['calls']), 1)
        c = d['calls'][0]
        self.assertEqual((c['opcode'], c['flags'], c['len'], c['off']),
                         (2, 0, 8, 32))  # DNSK_RW_WRITE

    def test_data_xchg_old_returned(self):
        d = run('data', 'xchg')
        self.assertEqual(d['RC'], '0')
        self.assertEqual(d['OLD'], '153')  # core old value returned via dh_rw_req
        self.assertEqual(d['calls'][0]['opcode'], 3)
        self.assertEqual(d['calls'][0]['flags'], 0)

    def test_data_cas_old_returned(self):
        d = run('data', 'cas')
        self.assertEqual(d['RC'], '0')
        self.assertEqual(d['calls'][0]['opcode'], 4)
        self.assertEqual(d['calls'][0]['flags'], 0)

    # ---- flag routing ----------------------------------------------------

    def test_text_write_uses_f_text(self):
        d = run('flags', 'text')
        self.assertEqual(d['RC'], '0')
        self.assertEqual(len(d['calls']), 1)
        self.assertEqual(d['calls'][0]['flags'], 2)   # DNSK_RW_F_TEXT
        self.assertEqual(d['calls'][0]['opcode'], 2)  # WRITE

    def test_ro_write_uses_f_ro(self):
        d = run('flags', 'ro')
        self.assertEqual(d['RC'], '0')
        self.assertEqual(d['calls'][0]['flags'], 4)   # DNSK_RW_F_RO

    # ---- splitting: derived from geometry, never hardcoded ---------------

    def test_split_read_over_256_chunks(self):
        d = run('split', 'read', str(SPLIT_START), str(SPLIT_LEN))
        self.assertEqual(d['RC'], '0')
        expected = self.assert_split(d, SPLIT_START, SPLIT_LEN, 1)
        self.assertGreater(len(expected), 1, 'the case must actually split')
        self.assertEqual(d['SCRATCH0'], '165')  # 0xA5 read from mock memory

    def test_split_read_from_zero_offset(self):
        """The default geometry: offset 0 relative to the mock buffer."""
        d = run('split', 'read', '0', str(SPLIT_LEN))
        self.assertEqual(d['RC'], '0')
        expected = self.assert_split(d, 0, SPLIT_LEN, 1)
        self.assertGreater(len(expected), 1, 'the case must actually split')

    def test_split_read_from_page_aligned_address(self):
        """Absolute address on a page boundary: full chunks come first."""
        d0 = run('split', 'read', '0', '1')
        page, chunk_max, _ = self.geometry(d0)
        start = self.start_for_page_off(d0, 0)
        d = run('split', 'read', str(start), str(SPLIT_LEN))
        self.assertEqual(d['RC'], '0')
        expected = self.assert_split(d, start, SPLIT_LEN, 1)
        self.assertEqual(expected[0], min(chunk_max, SPLIT_LEN),
                         'a page-aligned start must take a full chunk')

    def test_split_read_from_unaligned_start(self):
        """Absolute page offset 40: the first chunk must stop at the
        boundary, not at the chunk max."""
        d0 = run('split', 'read', '0', '1')
        page, chunk_max, _ = self.geometry(d0)
        start = self.start_for_page_off(d0, 40)
        d = run('split', 'read', str(start), str(SPLIT_LEN))
        self.assertEqual(d['RC'], '0')
        expected = self.assert_split(d, start, SPLIT_LEN, 1)
        self.assertEqual(expected[0], min(page - 40, chunk_max, SPLIT_LEN),
                         'the first chunk must stop at the page boundary')
        self.assertGreater(len(expected), 1)

    def test_split_read_from_last_byte_of_page(self):
        """One byte before the boundary: the first chunk must be exactly 1."""
        d0 = run('split', 'read', '0', '1')
        page, _, _ = self.geometry(d0)
        start = self.start_for_page_off(d0, page - 1)
        d = run('split', 'read', str(start), str(SPLIT_LEN))
        self.assertEqual(d['RC'], '0')
        expected = self.assert_split(d, start, SPLIT_LEN, 1)
        self.assertEqual(expected[0], 1,
                         'a start at page-1 must yield a single-byte chunk')

    # ---- splitting: write path (save originals, then write) --------------

    def test_split_write_saves_then_writes(self):
        d = run('split', 'write', str(SPLIT_START), str(SPLIT_LEN))
        self.assertEqual(d['RC'], '0')
        nr = len(self.assert_split(d, SPLIT_START, SPLIT_LEN, 1))
        self.assert_split(d, SPLIT_START, SPLIT_LEN, 2)
        ops = [c['opcode'] for c in d['calls']]
        self.assertEqual(ops, [1] * nr + [2] * nr,
                         'every chunk is saved with READ before any WRITE')
        self.assert_memory_pattern(d, SPLIT_START, SPLIT_LEN, written=True)

    def test_split_write_from_unaligned_start_lands_bytes(self):
        """Non-aligned write must land every byte, across the short
        boundary chunk the planner emits mid-transfer."""
        d0 = run('split', 'read', '0', '1')
        start = self.start_for_page_off(d0, 40)
        d = run('split', 'write', str(start), str(SPLIT_LEN))
        self.assertEqual(d['RC'], '0')
        nr = len(self.assert_split(d, start, SPLIT_LEN, 1))
        self.assert_split(d, start, SPLIT_LEN, 2)
        self.assertEqual([c['opcode'] for c in d['calls']], [1] * nr + [2] * nr)
        self.assert_memory_pattern(d, start, SPLIT_LEN, written=True)

    # ---- splitting: failure handling -------------------------------------

    def test_split_fails_in_save_phase_writes_nothing(self):
        d0 = run('split', 'read', str(SPLIT_START), str(SPLIT_LEN))
        nr = len(self.chunk_spans(d0, 1))
        d = run('split', 'fail', str(SPLIT_START), str(SPLIT_LEN), str(nr))
        self.assertNotEqual(d['RC'], '0')
        self.assertEqual(d['calls'][-1]['ret'], -5)  # injected failure
        self.assertFalse(any(c['opcode'] == 2 for c in d['calls']),
                         'no WRITE must be issued when save_originals fails')
        self.assert_memory_pattern(d, SPLIT_START, SPLIT_LEN, written=False)

    def test_split_failure_in_write_restores_written_chunks(self):
        d0 = run('split', 'read', str(SPLIT_START), str(SPLIT_LEN))
        nr = len(self.chunk_spans(d0, 1))
        # Save phase is calls 1..nr; the second WRITE (call nr+2) fails.
        d = run('split', 'fail_write', str(SPLIT_START), str(SPLIT_LEN),
                str(nr + 2))
        self.assertEqual(d['RC'], '-5')  # injected errno propagates
        writes = [c for c in d['calls'] if c['opcode'] == 2]
        # chunk1 written, chunk2 failed, then reverse restore of chunk1.
        self.assertEqual(len(writes), 3)
        self.assertEqual(writes[0]['off'], SPLIT_START)
        self.assertEqual(writes[1]['ret'], -5)
        self.assertEqual(writes[2]['off'], SPLIT_START)  # restore first chunk
        self.assert_memory_pattern(d, SPLIT_START, SPLIT_LEN, written=False)

    # ---- resolve / patch_pointer / fini ----------------------------------

    def test_probe_resolves_by_name(self):
        d = run('probe')
        self.assertEqual(d['PROBE'], '0xcafebabe')
        self.assertEqual(d['PROBE_MISS'], '(nil)')

    def test_patch_pointer_maps_to_core(self):
        d = run('patch_pointer')
        self.assertEqual(d['RC'], '0')
        self.assertEqual(d['SLOT'], '0x1234')

    def test_fini_clears_and_rejects_double_fini(self):
        d = run('fini')
        self.assertEqual(d['INIT'], '0')
        self.assertEqual(d['FINI'], '0')
        self.assertEqual(d['FINI2'], '-22')
        self.assertIn('name=(null)', d['BACKEND'])


if __name__ == '__main__':
    unittest.main()
