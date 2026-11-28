# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import build_flags  # noqa: E402
import exports_guard  # noqa: E402
import kmi_matrix  # noqa: E402
import package  # noqa: E402


class KmiMatrixTests(unittest.TestCase):
    def test_matrix_has_eight_unique_kmis(self):
        self.assertEqual(len(kmi_matrix.KMIS), 8)
        self.assertEqual(len(set(kmi_matrix.KMIS)), 8)

    def test_profiles_are_scaffold_only(self):
        profiles = sorted((ROOT / "profiles").glob("android*.json"))
        self.assertEqual(len(profiles), 8)
        features = {"core", "ns", "devtmpfs", "netfilter", "ipset", "cgpids",
                    "userns", "tmpfsx"}
        for path in profiles:
            data = json.loads(path.read_text())
            self.assertFalse(data["validated"], path)
            self.assertEqual(data["schema"], 2, path)
            if path.stem == "android16-6.12":
                self.assertEqual(data["stage"], "M1-ns-development", path)
            else:
                self.assertEqual(data["stage"], "M0-core-scaffold", path)
            self.assertEqual(set(data["features"]), features, path)
            for name, feature in data["features"].items():
                # Implementation status is recorded in docs/status.md only.
                self.assertNotIn("status", feature, path)
                self.assertIn("target_level", feature, path)


class BuildFlagsTests(unittest.TestCase):
    def test_xt_profiles(self):
        root = ROOT / "profiles" / "abi"
        pid, kmi, config = build_flags.load(root / "android16-6.12-xt104.json")
        self.assertEqual(pid, "android16-6.12-xt104")
        self.assertEqual(kmi, "android16-6.12")
        flags = build_flags.flags_for(pid, config)
        self.assertEqual(flags["DNSK_ABI_PROFILE"], "android16-6.12-xt104")
        self.assertEqual(flags["DNSK_XT_NOCOMPAT"], "1")
        self.assertEqual(flags["DNSK_XT_LAYOUT"], "104")
        self.assertEqual(flags["DNSK_PAGE_SIZE"], "4096")
        self.assertEqual(flags["DNSK_MAX_SKB_FRAGS"], "17")
        self.assertEqual(flags["DNSK_CGROUP_SUBSYS_COUNT"], "8")
        self.assertEqual(flags["DNSK_CGROUP_SUBSYS_LAYOUT"], "256")
        self.assertEqual(flags["DNSK_CGROUP_SUBSYS_CAN_FORK"], "104")
        self.assertEqual(flags["DNSK_CGROUP_SUBSYS_SLOT"], "7")
        self.assertEqual(flags["DNSK_CGROUP_SUBSYS_LEGACY_NAME"], "168")
        self.assertEqual(flags["DNSK_CGROUP_HAS_CSS_KILLED"], "0")

        pid, kmi, config = build_flags.load(root / "android16-6.12-xt128.json")
        self.assertEqual(pid, "android16-6.12-xt128")
        self.assertEqual(kmi, "android16-6.12")
        flags = build_flags.flags_for(pid, config)
        self.assertEqual(flags["DNSK_ABI_PROFILE"], "android16-6.12-xt128")
        self.assertEqual(flags["DNSK_XT_NOCOMPAT"], "")
        self.assertEqual(flags["DNSK_XT_LAYOUT"], "128")
        self.assertEqual(flags["DNSK_CGROUP_SUBSYS_LAYOUT"], "264")
        self.assertEqual(flags["DNSK_CGROUP_SUBSYS_CAN_FORK"], "112")
        self.assertEqual(flags["DNSK_CGROUP_HAS_CSS_KILLED"], "1")

    def test_profile_rejects_unknown_or_missing_config(self):
        text = (ROOT / "profiles/abi/android16-6.12-xt104.json").read_text()
        data = json.loads(text)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "profile.json"
            for mutate in ("unknown", "missing"):
                broken = json.loads(json.dumps(data))
                if mutate == "unknown":
                    broken["config"]["not_a_knob"] = 1
                else:
                    del broken["config"]["page_size"]
                path.write_text(json.dumps(broken))
                with self.assertRaises(ValueError):
                    build_flags.load(path)

    def test_profile_kmi_mismatch(self):
        argv = ["build_flags.py", "--profile",
                str(ROOT / "profiles/abi/android16-6.12-xt104.json"),
                "--kmi", "android14-6.1"]
        with mock.patch.object(sys, "argv", argv), \
                mock.patch("sys.stderr"), \
                mock.patch("builtins.print"):
            self.assertEqual(build_flags.main(), 2)


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


class PackageStampTests(unittest.TestCase):
    def test_stamp_keys_per_module(self):
        for stem in ("dnsk_ipset", "dnsk_netfilter"):
            self.assertIn("dnsk_xt_layout", package.stamp_keys(stem))
        for stem in ("dnsk_ns", "dnsk_devtmpfs"):
            self.assertNotIn("dnsk_xt_layout", package.stamp_keys(stem))
            self.assertIn("dnsk_abi_profile", package.stamp_keys(stem))

    def test_policy_and_modinfo_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "policy.conf").write_text(
                "# comment\nabi_profile=android16-6.12-xt104\nxt_layout=104\n"
            )
            self.assertEqual(
                package.policy_value(root, "abi_profile"),
                "android16-6.12-xt104",
            )
            self.assertEqual(package.policy_value(root, "xt_layout"), "104")
            self.assertIsNone(package.policy_value(root, "missing"))

            ko = root / "dnsk_ipset.ko"
            ko.write_bytes(
                b"\x7fELF\0dnsk_xt_layout=104\0dnsk_abi_profile=android16-6.12-xt104\0"
            )
            self.assertEqual(package.modinfo_value(ko, "dnsk_xt_layout"), "104")
            self.assertEqual(
                package.modinfo_value(ko, "dnsk_abi_profile"),
                "android16-6.12-xt104",
            )
            self.assertIsNone(package.modinfo_value(ko, "dnsk_missing"))


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
        # Assert the honest claims, not a milestone label that has since moved:
        # the artifact must say it is a dev build, that mqueue was verified, and
        # that no release is supported.
        self.assertIn("dev artifact", text)
        self.assertIn("mqueue verified", text)
        self.assertIn("No release support", text)


class LoaderReuseTests(unittest.TestCase):
    """The lkmins backend must replace only the load/unload call itself."""

    def test_unload_follows_the_selected_loader(self):
        text = (ROOT / "tools" / "dnskctl").read_text()
        self.assertIn('"$lkmctl" rmmod "$name"', text)
        self.assertIn('"$bb" rmmod "$name"', text)
        # The safety gates are what make dnskctl's unload different from a bare
        # rmmod; they must run on both paths.
        for gate in ("module_holders", "quiesce", "dmesg_alarms_since"):
            self.assertIn(gate, text)

    def test_default_loader_is_lkmins(self):
        text = (ROOT / "tools" / "dnskctl").read_text()
        self.assertNotIn("${DNSK_LOADER:-ksud}", text)
        self.assertGreaterEqual(text.count("${DNSK_LOADER:-lkmins}"), 4)

    def test_package_requires_and_ships_lkmctl(self):
        text = (ROOT / "tools" / "package.py").read_text()
        self.assertIn('"--lkmctl"', text)
        self.assertIn("required=True", text)
        self.assertIn('"bin/lkmctl"', text)

    def test_unload_command_doc_is_one_complete_line(self):
        lines = (ROOT / "tools" / "dnskctl").read_text().splitlines()
        declared = [line for line in lines if "@command unload" in line]
        self.assertEqual(len(declared), 1)
        self.assertIn("quiesce", declared[0])
        self.assertIn("post-check", declared[0])
        page = (ROOT / "docs" / "api" / "userspace.md").read_text()
        rows = [line for line in page.splitlines()
                if line.startswith("| `unload <feature>`")]
        self.assertEqual(len(rows), 1)
        self.assertIn("quiesce", rows[0])
        self.assertIn("post-check", rows[0])


class UnloadLoaderValidationTests(unittest.TestCase):
    def test_unload_rejects_unknown_loader(self):
        sh = shutil.which("sh")
        if not sh:
            self.skipTest("no POSIX shell")
        with tempfile.TemporaryDirectory() as td:
            bindir = Path(td) / "bin"
            bindir.mkdir()
            # need_root() runs before loader validation; report uid 0 so the
            # test reaches the branch under test without real privileges.
            fake_id = bindir / "id"
            fake_id.write_text("#!/bin/sh\necho 0\n")
            fake_id.chmod(0o755)
            env = dict(os.environ)
            env["PATH"] = f"{bindir}:{env.get('PATH', '')}"
            env["DNSK_LOADER"] = "bogus"
            proc = subprocess.run(
                [sh, str(ROOT / "tools" / "dnskctl"), "unload", "core"],
                text=True, capture_output=True, env=env, check=False,
            )
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("DNSK_LOADER must be ksud or lkmins", proc.stderr)


if __name__ == "__main__":
    unittest.main()
