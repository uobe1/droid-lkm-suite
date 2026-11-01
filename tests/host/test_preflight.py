# SPDX-License-Identifier: GPL-3.0-or-later
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def write_btf(path: Path, match_size: int, target_size: int) -> None:
    import struct

    strs = b"\0xt_match\0xt_target\0"
    name_match = 1
    name_target = len(b"\0xt_match\0")
    types = b"".join(
        struct.pack("<III", name, (4 << 24) | 0, size)
        for name, size in ((name_match, match_size), (name_target, target_size))
    )
    header = struct.pack(
        "<HBBIIIII",
        0xEB9F, 1, 0, 24, 0, len(types), len(types), len(strs),
    )
    path.write_bytes(header + types + strs)


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

    def test_preflight_xt_btf_layout(self):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if not cc:
            self.skipTest("no host C compiler")
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            binary = tmp / "dnsk-preflight"
            subprocess.run(
                [cc, "-O2", "-Wall", "-Wextra", "-std=c11", "-D_GNU_SOURCE",
                 "-o", str(binary), str(ROOT / "tools/dnsk_preflight.c")],
                check=True,
            )
            btf = tmp / "vmlinux.btf"
            write_btf(btf, 104, 104)
            proc = subprocess.run(
                [str(binary), "--btf", str(btf), "--xt-layout", "104",
                 "--kmi", "android16-6.12",
                 "--supported", str(ROOT / "ksu/supported_kmis.txt")],
                text=True, capture_output=True, check=False,
            )
            data = json.loads(proc.stdout)
            self.assertEqual(data["btf_layout"]["xt_match_size"], 104)
            self.assertEqual(data["btf_layout"]["xt_target_size"], 104)
            self.assertTrue(data["btf_layout"]["xt_ok"])
            self.assertTrue(data["checks"]["xt_layout"])

            write_btf(btf, 128, 128)
            proc = subprocess.run(
                [str(binary), "--btf", str(btf), "--xt-layout", "104",
                 "--kmi", "android16-6.12",
                 "--supported", str(ROOT / "ksu/supported_kmis.txt")],
                text=True, capture_output=True, check=False,
            )
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["xt_ok"])
            self.assertFalse(data["checks"]["xt_layout"])


if __name__ == "__main__":
    unittest.main()
