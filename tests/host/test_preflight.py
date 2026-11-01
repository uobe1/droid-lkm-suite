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
    ("nf_hook_state", 48, [
        ("hook", 0), ("pf", 1), ("in", 8), ("out", 16), ("sk", 24),
        ("net", 32), ("okfn", 40),
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
    ("nf_nat_hook", 32, [
        ("parse_nat_setup", 0), ("decode_session", 8),
        ("remove_nat_bysrc", 16),
    ]),
    ("nf_nat_range2", 44, [
        ("flags", 0), ("min_addr", 4), ("max_addr", 20),
        ("min_proto", 36), ("max_proto", 38), ("base_proto", 40),
    ]),
    ("rhashtable", 160, []),
]

BOUNDARY_STRUCTS = [
    ("mutex", 48, [
        ("owner", 0), ("wait_lock", 8), ("wait_list", 16),
        ("android_oem_data1", 32),
    ]),
    ("rw_semaphore", 64, [
        ("count", 0), ("owner", 8), ("wait_list", 24),
        ("android_vendor_data1", 40), ("android_oem_data1", 48),
    ]),
    ("spinlock", 4, []),
    ("raw_spinlock", 4, [("raw_lock", 0)]),
    ("wait_queue_head", 24, [("lock", 0), ("head", 8)]),
    ("timer_list", 40, [
        ("entry", 0), ("expires", 16), ("function", 24), ("flags", 32),
    ]),
    ("work_struct", 32, [("data", 0), ("entry", 8), ("func", 24)]),
    ("xt_action_param", 32, [
        ("state", 16), ("thoff", 24), ("fragoff", 28), ("hotdrop", 30),
    ]),
    ("xt_mtchk_param", 48, [
        ("net", 0), ("table", 8), ("entryinfo", 16), ("match", 24),
        ("matchinfo", 32), ("hook_mask", 40), ("family", 44),
        ("nft_compat", 45),
    ]),
    ("xt_tgchk_param", 48, [
        ("net", 0), ("table", 8), ("entryinfo", 16), ("target", 24),
        ("targinfo", 32), ("hook_mask", 40), ("family", 44),
        ("nft_compat", 45),
    ]),
    ("sk_buff", 248, [
        ("sk", 24), ("_nfct", 104), ("len", 112), ("data_len", 116),
        ("active_extensions", 127), ("tail", 208), ("end", 212),
        ("head", 216), ("data", 224), ("truesize", 232),
        ("users", 236), ("extensions", 240),
    ]),
    ("skb_shared_info", 360, [
        ("flags", 0), ("nr_frags", 2), ("frag_list", 8),
        ("gso_type", 24), ("dataref", 32), ("destructor_arg", 40),
        ("frags", 88),
    ]),
    ("net", 4160, [
        ("user_ns", 80), ("proc_net", 160), ("sysctls", 176),
        ("ipv4", 768), ("ipv6", 1536), ("nf", 2384), ("ct", 2608),
        ("wext_nlevents", 2824), ("gen", 2848), ("xfrm", 2944),
    ]),
]


FS_STRUCTS = [
    ("device", 912, [("kobj", 0), ("devt", 760)]),
    ("kobject", 96, [("entry", 8)]),
    ("kset", 160, [("list", 0), ("list_lock", 16)]),
    ("super_block", 1536, [
        ("s_root", 104), ("s_umount", 112), ("s_active", 180),
    ]),
    ("inode", 704, [
        ("i_mode", 0), ("i_rdev", 76), ("i_private", 696),
    ]),
    ("kstat", 184, [("mode", 4), ("rdev", 44)]),
    ("vfsmount", 64, [("mnt_root", 0), ("mnt_sb", 8)]),
    ("path", 16, [("mnt", 0), ("dentry", 8)]),
    ("file_system_type", 72, [
        ("name", 0), ("fs_flags", 8), ("mount", 32), ("kill_sb", 40),
        ("owner", 48),
    ]),
]


NS_STRUCTS = [
    ("task_struct", 5184, [
        ("pid", 1800), ("tgid", 1804), ("real_parent", 1816),
        ("parent", 1824), ("group_leader", 1864), ("thread_pid", 1904),
        ("nsproxy", 2384), ("signal", 2392), ("sighand", 2400),
        ("real_cred", 2296), ("cred", 2304), ("cgroups", 2872),
        ("cg_list", 2880), ("security", 3288), ("comm", 2320),
        ("fs", 2360), ("files", 2368), ("mm", 1672),
        ("active_mm", 1680),
    ]),
    ("nsproxy", 72, [
        ("uts_ns", 8), ("ipc_ns", 16), ("mnt_ns", 24),
        ("pid_ns_for_children", 32), ("net_ns", 40), ("time_ns", 48),
        ("time_ns_for_children", 56), ("cgroup_ns", 64),
    ]),
    ("pid_namespace", 136, [
        ("idr", 0), ("child_reaper", 48), ("level", 64), ("parent", 72),
        ("user_ns", 80), ("ucounts", 88), ("ns", 104),
        ("memfd_noexec_scope", 128),
    ]),
    ("ipc_namespace", 1264, [
        ("ids", 0), ("sem_ctls", 792), ("mq_mnt", 960),
        ("mq_sysctls", 1096), ("ipc_sysctls", 1208), ("user_ns", 1216),
        ("ucounts", 1224), ("ns", 1240),
    ]),
    ("user_namespace", 632, [
        ("uid_map", 0), ("gid_map", 64), ("parent", 192), ("level", 200),
        ("ns", 216), ("ucounts", 488),
    ]),
    ("cred", 184, [
        ("uid", 8), ("euid", 24), ("security", 128), ("user_ns", 144),
        ("ucounts", 152),
    ]),
    ("pid", 112, [("count", 0), ("level", 4), ("tasks", 32)]),
    ("upid", 16, [("nr", 0), ("ns", 8)]),
    ("ns_common", 24, [("stashed", 0), ("ops", 8), ("inum", 16)]),
    ("signal_struct", 1120, [
        ("sigcnt", 0), ("nr_threads", 8), ("thread_head", 16),
        ("shared_pending", 64),
    ]),
    ("sighand_struct", 2080, [("siglock", 0), ("action", 32)]),
    ("cgroup", 1984, [
        ("self", 0), ("flags", 216), ("level", 224), ("subsys", 616),
        ("nr_dying_subsys", 680), ("e_csets", 736), ("psi", 1176),
        ("bpf", 1184), ("freezer", 1904), ("ancestors", 1936),
    ]),
    ("css_set", 456, [
        ("subsys", 0), ("refcount", 64), ("dfl_cgrp", 80),
        ("cgrp_links", 336),
    ]),
]


CGPIDS_STRUCTS = [
    ("cgroup_subsys", 256, [
        ("can_attach", 72), ("attach", 88), ("can_fork", 104),
        ("cancel_fork", 112), ("fork", 120), ("exit", 128),
        ("release", 136), ("bind", 144),
    ]),
    ("cgroup", 1984, [
        ("self", 0), ("flags", 216), ("level", 224), ("subsys", 616),
        ("nr_dying_subsys", 680), ("e_csets", 736), ("psi", 1176),
        ("bpf", 1184), ("freezer", 1904), ("ancestors", 1936),
    ]),
    ("css_set", 456, [
        ("subsys", 0), ("refcount", 64), ("dfl_cgrp", 80),
        ("cgrp_links", 336),
    ]),
]


def mutate(structs, struct_name, member, offset):
    return [
        (name, size,
         [(member, offset) if (name == struct_name and m == member)
          else (m, off) for m, off in rows])
        for name, size, rows in structs
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
        "targetsize", "usersize", "compat_from_user", "compat_to_user",
        "compatsize", "hooks", "proto", "family",
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
            rows.append(("compat_from_user", 72))
            rows.append(("compat_to_user", 80))
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
            extra = NF_STRUCTS + BOUNDARY_STRUCTS
            write_btf(btf, 104, 104, extra=extra)
            base = [str(binary), "--btf", str(btf), "--xt-layout", "104",
                    "--feature", "netfilter", "--kmi", "android16-6.12",
                    "--supported", str(ROOT / "ksu/supported_kmis.txt")]
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            nf = data["btf_layout"]["nf"]
            boundary = data["btf_layout"]["boundary"]
            self.assertTrue(nf["checked"])
            self.assertTrue(nf["hook_state"])
            self.assertTrue(nf["nat_hook"])
            self.assertTrue(nf["nat_range2"])
            self.assertTrue(nf["ok"])
            self.assertTrue(boundary["locks"])
            self.assertTrue(boundary["xt_params"])
            self.assertTrue(boundary["skb"])
            self.assertTrue(boundary["net"])
            self.assertTrue(boundary["ok"])
            self.assertTrue(data["checks"]["nf_abi"])
            self.assertTrue(data["checks"]["boundary"])

            broken = mutate(extra, "nf_nat_range2", "base_proto", 41)
            write_btf(btf, 104, 104, extra=broken)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["nf"]["nat_range2"])
            self.assertFalse(data["btf_layout"]["nf"]["ok"])
            self.assertFalse(data["checks"]["nf_abi"])
            self.assertTrue(data["checks"]["boundary"])

            bad_gen = mutate(extra, "net", "gen", 2856)
            write_btf(btf, 104, 104, extra=bad_gen)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["boundary"]["net"])
            self.assertFalse(data["btf_layout"]["boundary"]["ok"])
            self.assertFalse(data["checks"]["boundary"])

    def test_preflight_devtmpfs_btf_layout(self):
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
            extra = FS_STRUCTS + BOUNDARY_STRUCTS
            write_btf(btf, 104, 104, extra=extra)
            base = [str(binary), "--btf", str(btf), "--feature", "devtmpfs",
                    "--kmi", "android16-6.12",
                    "--supported", str(ROOT / "ksu/supported_kmis.txt")]
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            boundary = data["btf_layout"]["boundary"]
            self.assertTrue(boundary["checked_fs"])
            self.assertTrue(boundary["fs"])
            self.assertTrue(boundary["ok"])
            self.assertTrue(data["checks"]["boundary"])

            broken = mutate(extra, "device", "devt", 768)
            write_btf(btf, 104, 104, extra=broken)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            boundary = data["btf_layout"]["boundary"]
            self.assertFalse(boundary["fs"])
            self.assertFalse(boundary["ok"])
            self.assertFalse(data["checks"]["boundary"])

            broken = mutate(extra, "file_system_type", "mount", 40)
            write_btf(btf, 104, 104, extra=broken)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["boundary"]["fs"])
            self.assertFalse(data["checks"]["boundary"])

    def test_preflight_ns_btf_layout(self):
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
            extra = NS_STRUCTS + BOUNDARY_STRUCTS
            write_btf(btf, 104, 104, extra=extra)
            base = [str(binary), "--btf", str(btf), "--feature", "ns",
                    "--kmi", "android16-6.12",
                    "--supported", str(ROOT / "ksu/supported_kmis.txt")]
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            boundary = data["btf_layout"]["boundary"]
            self.assertTrue(boundary["checked_ns"])
            self.assertTrue(boundary["ns"])
            self.assertTrue(boundary["ok"])
            self.assertTrue(data["checks"]["boundary"])

            broken = mutate(extra, "task_struct", "nsproxy", 2392)
            write_btf(btf, 104, 104, extra=broken)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            boundary = data["btf_layout"]["boundary"]
            self.assertFalse(boundary["ns"])
            self.assertFalse(boundary["ok"])
            self.assertFalse(data["checks"]["boundary"])

            broken = mutate(extra, "nsproxy", "ipc_ns", 24)
            write_btf(btf, 104, 104, extra=broken)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["boundary"]["ns"])
            self.assertFalse(data["checks"]["boundary"])

    def test_preflight_cgpids_btf_layout(self):
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
            extra = CGPIDS_STRUCTS + BOUNDARY_STRUCTS
            write_btf(btf, 104, 104, extra=extra)
            base = [str(binary), "--btf", str(btf), "--feature", "cgpids",
                    "--kmi", "android16-6.12",
                    "--supported", str(ROOT / "ksu/supported_kmis.txt")]
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            boundary = data["btf_layout"]["boundary"]
            self.assertTrue(boundary["checked_cgpids"])
            self.assertTrue(boundary["cgpids"])
            self.assertTrue(boundary["ok"])
            self.assertTrue(data["checks"]["boundary"])

            bad_size = [(name, 264 if name == "cgroup_subsys" else size, rows)
                        for name, size, rows in extra]
            write_btf(btf, 104, 104, extra=bad_size)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["boundary"]["cgpids"])
            self.assertFalse(data["checks"]["boundary"])

            ddk = [(name, size, rows + [("css_killed", 48)])
                   if name == "cgroup_subsys" else (name, size, rows)
                   for name, size, rows in extra]
            write_btf(btf, 104, 104, extra=ddk)
            proc = subprocess.run(base, text=True, capture_output=True,
                                  check=False)
            data = json.loads(proc.stdout)
            self.assertFalse(data["btf_layout"]["boundary"]["cgpids"])
            self.assertFalse(data["checks"]["boundary"])


if __name__ == "__main__":
    unittest.main()
