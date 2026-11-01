# SPDX-License-Identifier: GPL-3.0-or-later
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


NF_STRUCTS = [
    ("nf_hook_ops", 40, [
        ("hook", 0), ("dev", 8), ("priv", 16), ("pf", 24),
        ("hook_ops_type", 25), ("hooknum", 28), ("priority", 32),
    ]),
    ("nf_conn", 272, [
        ("status", 128), ("nat_bysource", 144), ("master", 160),
        ("mark", 168), ("secmark", 172), ("ext", 176), ("proto", 184),
    ]),
    ("pernet_operations", 72, [
        ("init", 16), ("pre_exit", 24), ("exit", 32),
        ("exit_batch", 40), ("exit_batch_rtnl", 48), ("id", 56),
        ("size", 64),
    ]),
    ("nfnetlink_subsystem", 64, [
        ("owner", 24), ("commit", 32), ("abort", 40), ("valid_genid", 48),
    ]),
    ("nfnl_callback", 32, [
        ("call", 0), ("policy", 8), ("type", 16), ("attr_count", 20),
    ]),
    ("net", 4160, [("gen", 2848)]),
    ("rhashtable", 160, []),
]


def write_btf(path: Path, match_size: int, target_size: int,
              family_override: int | None = None,
              extra=()) -> None:
    import struct

    compat = match_size == 128
    me = 88 if compat else 72
    table = 96 if compat else 80
    size_off = 104 if compat else 88
    usersize = 108 if compat else 92
    compatsize = 112 if compat else None
    hooks = 116 if compat else 96
    proto = 120 if compat else 100
    family = 122 if compat else 102

    names = [
        "xt_match", "xt_target", "list", "name", "revision", "match",
        "target", "checkentry", "destroy", "me", "table", "matchsize",
        "targetsize", "usersize", "compatsize", "hooks", "proto", "family",
    ]
    for struct_name, _, rows in extra:
        names.append(struct_name)
        names.extend(member for member, _ in rows)
    strtab = bytearray(b"\0")
    offsets = {}
    for name in names:
        offsets[name] = len(strtab)
        strtab += name.encode() + b"\0"

    def member_rows(target: bool, fam: int) -> list[tuple[str, int]]:
        rows = [
            ("list", 0), ("name", 16), ("revision", 45),
            ("target" if target else "match", 48),
            ("checkentry", 56), ("destroy", 64), ("me", me),
            ("table", table),
            ("targetsize" if target else "matchsize", size_off),
            ("usersize", usersize),
        ]
        if compatsize is not None:
            rows.append(("compatsize", compatsize))
        rows += [("hooks", hooks), ("proto", proto), ("family", fam)]
        return rows

    types = bytearray()

    def add_struct(name: str, size: int, rows: list[tuple[str, int]]) -> None:
        types.extend(struct.pack("<III", offsets[name], (4 << 24) | len(rows), size))
        for member, off in rows:
            types.extend(struct.pack("<III", offsets[member], 1, off * 8))

    match_family = family if family_override is None else family_override
    add_struct("xt_match", match_size, member_rows(False, match_family))
    add_struct("xt_target", target_size, member_rows(True, family))
    for struct_name, size, rows in extra:
        add_struct(struct_name, size, rows)
    header = struct.pack(
        "<HBBIIIII",
        0xEB9F, 1, 0, 24, 0, len(types), len(types), len(strtab),
    )
    path.write_bytes(header + bytes(types) + bytes(strtab))


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
                [str(binary), "--btf", str(btf), "--xt-layout", "128",
                 "--kmi", "android16-6.12",
                 "--supported", str(ROOT / "ksu/supported_kmis.txt")],
                text=True, capture_output=True, check=False,
            )
            data = json.loads(proc.stdout)
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
            self.assertNotEqual(proc.returncode, 0)

            write_btf(btf, 104, 104, family_override=100)
            proc = subprocess.run(
                [str(binary), "--btf", str(btf), "--xt-layout", "104",
                 "--kmi", "android16-6.12",
                 "--supported", str(ROOT / "ksu/supported_kmis.txt")],
                text=True, capture_output=True, check=False,
            )
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["xt_ok"])
            self.assertFalse(data["checks"]["xt_layout"])
            self.assertNotEqual(proc.returncode, 0)

    def test_preflight_netfilter_btf_layout(self):
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
            write_btf(btf, 104, 104, extra=NF_STRUCTS)
            base = [str(binary), "--btf", str(btf), "--xt-layout", "104",
                    "--feature", "netfilter", "--kmi", "android16-6.12",
                    "--supported", str(ROOT / "ksu/supported_kmis.txt")]
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            nf = data["btf_layout"]["nf"]
            self.assertTrue(nf["checked"])
            self.assertTrue(nf["ok"])
            self.assertTrue(nf["net"])
            self.assertTrue(data["checks"]["nf_abi"])

            broken = [(name, 4152 if name == "net" else size, rows)
                      for name, size, rows in NF_STRUCTS]
            write_btf(btf, 104, 104, extra=broken)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            nf = data["btf_layout"]["nf"]
            self.assertFalse(nf["net"])
            self.assertFalse(nf["ok"])
            self.assertFalse(data["checks"]["nf_abi"])

            bad_gen = [(name, size,
                        [("gen", 2856) if member == "gen" else (member, off)
                         for member, off in rows]
                        if name == "net" else rows)
                       for name, size, rows in NF_STRUCTS]
            write_btf(btf, 104, 104, extra=bad_gen)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["nf"]["net"])
            self.assertFalse(data["checks"]["nf_abi"])


if __name__ == "__main__":
    unittest.main()
