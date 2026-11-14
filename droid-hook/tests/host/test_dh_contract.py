#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host selftest for the Droid-Hook library contract.

Compiles dh_header_probe.c with the host compiler (no kernel headers), runs it,
and asserts the public ABI constants, struct layout and kernel-doc coverage of
dh.h.  This is a host-only gate: it never builds a kernel module and never
needs a kernel tree.
"""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INC = ROOT / 'include'
PROBE = ROOT / 'tests' / 'host' / 'dh_header_probe.c'
HEADER = INC / 'dh.h'
BIN = Path('/tmp/dh_header_probe')


def compile_and_run():
    cc = 'cc'
    subprocess.run(
        [cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
         '-I', str(INC), '-o', str(BIN), str(PROBE)],
        check=True, capture_output=True, text=True)
    return subprocess.run([str(BIN)], check=True,
                          capture_output=True, text=True).stdout


class DhContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = compile_and_run()

    def test_public_constants(self):
        self.assertIn('DH_ABI=1 DH_KEY_LEN=48 DH_UNDO_SLOTS=8 DH_MAX_BATCH=64',
                      self.out)

    def test_structs_nonempty(self):
        sizes = dict(re.findall(r'sizeof\((\w+)\)=(\d+)', self.out))
        for name in ('ctx', 'be', 'txn', 'undo', 'req', 'hook'):
            self.assertIn(name, sizes)
            self.assertGreater(int(sizes[name]), 0, name)

    def test_field_offsets_stable(self):
        self.assertIn('offsetof(dh_txn,n)=', self.out)
        m = re.search(r'offsetof\(dh_rw_req,ret\)=(\d+)', self.out)
        self.assertIsNotNone(m)
        self.assertGreater(int(m.group(1)), 0)
        self.assertRegex(self.out, r'shadow_data_offset=\d+')

    def test_route_and_ret_values(self):
        self.assertIn('route_alias=1 route_dma=4 ret_verify=-1000', self.out)

    def test_every_public_declaration_has_kernel_doc(self):
        text = HEADER.read_text(encoding='utf-8')
        # Documented declaration headers: "define X -", "struct X -",
        # "enum X -", "DOC:" section, "X() -".
        documented = set()
        for m in re.finditer(r'/\*\*(.*?)\*/', text, re.S):
            first = next((l.strip().lstrip('*').strip()
                          for l in m.group(1).splitlines() if l.strip()), '')
            d = re.match(r'^(?:define|struct|enum|union|typedef)\s+([A-Za-z_]\w*)', first)
            f = re.match(r'^([A-Za-z_]\w*)\(\)', first)
            if d:
                documented.add(d.group(1))
            elif f:
                documented.add(f.group(1))
        missing = []
        for i, line in enumerate(text.splitlines(), 1):
            s = line.strip()
            m = re.match(r'^(?:#define|struct|enum|typedef|union)\s+([A-Za-z_]\w*)', s)
            if m and not s.startswith('#define _'):
                name = m.group(1)
                if name not in documented and not s.endswith(';'):
                    missing.append(f'{i}:{name}')
        self.assertEqual(missing, [], f'undocumented declarations: {missing}')

    def test_no_kernel_only_include_leaks_to_host(self):
        text = HEADER.read_text(encoding='utf-8')
        # linux/types.h must be inside the __KERNEL__ guard.
        kernel_guard = text.index('#ifdef __KERNEL__')
        host_else = text.index('#else', kernel_guard)
        self.assertLess(text.index('#include <linux/types.h>'), host_else)


if __name__ == '__main__':
    unittest.main()
