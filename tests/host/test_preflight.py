# SPDX-License-Identifier: GPL-3.0-or-later
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class PreflightTests(unittest.TestCase):
    def test_preflight_json_on_host(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("no host C compiler")
        with tempfile.TemporaryDirectory() as td:
            binary = Path(td) / "dnsk-preflight"
            subprocess.run(
                [cc, "-O2", "-Wall", "-Wextra", "-std=c11", "-D_GNU_SOURCE",
                 "-o", str(binary), str(ROOT / "tools/dnsk_preflight.c")],
                check=True,
            )
            proc = subprocess.run(
                [str(binary), "--kmi", "android16-6.12",
                 "--supported", str(ROOT / "ksu/supported_kmis.txt")],
                text=True, capture_output=True, check=False,
            )
        data = json.loads(proc.stdout)
        self.assertEqual(data["kmi"], "android16-6.12")
        self.assertTrue(data["checks"]["page_size_4k"])
        self.assertIn("sys_call_table", data["symbols"])
        self.assertIn("init_mm", data["symbols"])
        self.assertIn("dcache", data["checks"])
        # ok may be false on a host without ksud; the JSON must still be valid.
        self.assertIn("ok", data)


if __name__ == "__main__":
    unittest.main()
