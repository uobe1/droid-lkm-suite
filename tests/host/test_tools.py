# SPDX-License-Identifier: GPL-3.0-or-later
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import exports_guard  # noqa: E402
import kmi_matrix  # noqa: E402


class KmiMatrixTests(unittest.TestCase):
    def test_matrix_has_eight_unique_kmis(self):
        self.assertEqual(len(kmi_matrix.KMIS), 8)
        self.assertEqual(len(set(kmi_matrix.KMIS)), 8)

    def test_profiles_are_scaffold_only(self):
        profiles = sorted((ROOT / "profiles").glob("android*.json"))
        self.assertEqual(len(profiles), 8)
        for path in profiles:
            data = json.loads(path.read_text())
            self.assertFalse(data["validated"], path)
            if path.stem == "android16-6.12":
                self.assertEqual(data["stage"], "M1-ns-development", path)
                self.assertEqual(data["features"]["ns"]["status"], "development", path)
                self.assertEqual(data["features"]["devtmpfs"]["status"], "development", path)
            else:
                self.assertEqual(data["stage"], "M0-core-scaffold", path)
                self.assertEqual(data["features"]["ns"]["status"], "not-shipped", path)
                self.assertEqual(data["features"]["devtmpfs"]["status"], "not-shipped", path)
            self.assertEqual(data["features"]["core"]["status"], "scaffold", path)
            for name, feature in data["features"].items():
                if name in ("core", "ns", "devtmpfs") and path.stem == "android16-6.12":
                    continue
                if name != "core":
                    self.assertEqual(feature["status"], "not-shipped", path)


class ExportsGuardTests(unittest.TestCase):
    def test_symvers_parser(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Module.symvers"
            path.write_text(
                "0x12345678\tfoo\tvmlinux\tEXPORT_SYMBOL\n"
                "0x0abcdef0\tbar\tsome_module\tEXPORT_SYMBOL_GPL\n"
                "not a symvers line\n"
            )
            self.assertEqual(exports_guard.exported_symbols(path), {"foo", "bar"})


class IpsetSourceTests(unittest.TestCase):
    def test_xt_set_arrays_are_complete(self):
        import re

        text = (
            ROOT
            / "features/ipset/vendor/linux/net/netfilter/xt_set.c"
        ).read_text()
        match_block = text.split("static struct xt_match set_matches", 1)[1].split("};", 1)[0]
        target_block = text.split("static struct xt_target set_targets", 1)[1].split("};", 1)[0]

        self.assertEqual(len(re.findall(r'\.name\s*=\s*"set"', match_block)), 9)
        self.assertEqual(len(re.findall(r'\.match\s*=\s*set_match_', match_block)), 9)
        self.assertEqual(len(re.findall(r'\.family\s*=\s*NFPROTO_IPV[46]', match_block)), 9)
        self.assertEqual(len(re.findall(r'\.me\s*=\s*THIS_MODULE', match_block)), 9)

        self.assertEqual(len(re.findall(r'\.name\s*=\s*"SET"', target_block)), 7)
        self.assertEqual(len(re.findall(r'\.target\s*=\s*set_target_', target_block)), 7)
        self.assertEqual(len(re.findall(r'\.family\s*=\s*NFPROTO_IPV[46]', target_block)), 7)
        self.assertEqual(len(re.findall(r'\.me\s*=\s*THIS_MODULE', target_block)), 7)


class PackagingInputTests(unittest.TestCase):
    def test_required_packaging_files_exist(self):
        for rel in (
            "ksu/module.prop",
            "ksu/post-fs-data.sh",
            "ksu/bin/panic_decode.sh",
            "tools/dnskctl",
            "tools/dnsk_preflight.c",
        ):
            self.assertTrue((ROOT / rel).is_file(), rel)

    def test_module_prop_is_dev_and_honest(self):
        text = (ROOT / "ksu/module.prop").read_text()
        self.assertIn("version=v1.0.0dev0", text)
        self.assertIn("M1 dev", text)
        self.assertIn("mqueue smoke verified", text)
        self.assertIn("no release support", text)


if __name__ == "__main__":
    unittest.main()
