#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host selftest for the core RW ABI 5 -> struct dh_backend adapter.

Compiles droid-hook/runtime/dh_core_backend.c together with a mock-core
harness (no kernel headers), then drives every scenario and asserts the
adapter's behaviour: field conversion, data-path routing, F_TEXT/F_RO routing,
mandatory-primitive validation, >256B / cross-page splitting, failure recovery
and symbol resolution.

This is a host-only gate: it never builds a kernel module and never needs a
kernel tree.
"""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'runtime'
INC = ROOT / 'include'
HARNESS_SRC = ROOT / 'tests' / 'host' / 'dh_runtime_harness.c'
BACKEND_SRC = RUNTIME / 'dh_core_backend.c'
BIN = Path('/tmp/dh_runtime_harness_test')

COMPILE_CMD = [
    'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-I', str(INC), '-I', str(RUNTIME),
    str(HARNESS_SRC), str(BACKEND_SRC), '-o', str(BIN),
]


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


class DhRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        r = compile_harness()
        if r.returncode != 0:
            raise AssertionError(f'compile failed:\n{r.stdout}\n{r.stderr}')
        cls.compile_stderr = r.stderr

    def test_compile_clean(self):
        self.assertEqual(self.compile_stderr.strip(), '')

    def test_init_fills_mandatory_primitives(self):
        d = run('init')
        self.assertEqual(d['INIT'], '0')
        self.assertEqual(d['BACKEND'].split()[0], 'name=dnsk_core_rw5')
        self.assertIn('abi=1', d['BACKEND'])
        for field in ('rw=', 'text=', 'ro=', 'probe=', 'pp='):
            self.assertNotIn(f'{field}(nil)', d['BACKEND'])
        self.assertEqual(d['PAGE'], '256')

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

    def test_split_read_over_256_chunks(self):
        d = run('split', 'read')
        self.assertEqual(d['RC'], '0')
        # 600B from off=8 with page_size=256 -> 152 + 256 + 192
        self.assertEqual([c['len'] for c in d['calls']], [152, 256, 192])
        self.assertEqual([c['off'] for c in d['calls']], [8, 160, 416])
        self.assertTrue(all(c['len'] <= 256 for c in d['calls']))
        self.assertEqual(d['SCRATCH0'], '165')  # 0xA5 read from mock memory

    def test_split_write_saves_then_writes(self):
        d = run('split', 'write')
        self.assertEqual(d['RC'], '0')
        ops = [c['opcode'] for c in d['calls']]
        self.assertEqual(ops, [1, 1, 1, 2, 2, 2])  # 3 READ saves + 3 WRITEs
        writes = [c for c in d['calls'] if c['opcode'] == 2]
        self.assertEqual([c['len'] for c in writes], [152, 256, 192])
        self.assertEqual(d['MEM8'], '0')       # big[0]
        self.assertEqual(d['MEM263'], '255')   # 2nd chunk landed

    def test_split_fails_in_save_phase_writes_nothing(self):
        d = run('split', 'fail')
        self.assertNotEqual(d['RC'], '0')
        self.assertEqual(d['calls'][-1]['ret'], -5)  # injected failure
        self.assertFalse(any(c['opcode'] == 2 for c in d['calls']),
                         'no WRITE must be issued when save_originals fails')
        self.assertEqual(d['MEM8'], '165')  # memory untouched

    def test_split_failure_in_write_restores_written_chunks(self):
        d = run('split', 'fail_write')
        self.assertEqual(d['RC'], '-5')  # injected errno propagates
        writes = [c for c in d['calls'] if c['opcode'] == 2]
        # chunk1 written, chunk2 failed, then reverse restore of chunk1.
        self.assertEqual(len(writes), 3)
        self.assertEqual(writes[0]['off'], 8)
        self.assertEqual(writes[1]['ret'], -5)
        self.assertEqual(writes[2]['off'], 8)   # restore back to first chunk
        self.assertEqual(d['MEM8'], '165')      # 0xA5 restored, no half write

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
