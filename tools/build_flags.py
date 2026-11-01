#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a build ABI profile into build flags.

Profiles under profiles/abi/ describe the kernel config values that are not
frozen by the GKI KMI but change struct layouts used by our modules.  The
config block is descriptive and must be complete: a missing or unknown knob is
rejected instead of silently assuming a default.  Only knobs that can be
expressed as compile-time code macros are emitted as build flags; the rest
must be matched by selecting a kernel/DDK build with the same config.
"""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path

SUPPORTED_SCHEMA = 2

# Every key is required in every profile.  Values are from the device BTF /
# /proc/config.gz or from the matching DDK defconfig; see
# docs/evidence/abi-config-scope.md.
CONFIG_SPECS = {
    "xtables_compat": ("bool", None),
    "android_vendor_oem_data": ("bool", None),
    "preempt_rt": ("bool", None),
    "lockdep": ("bool", None),
    "debug_mutexes": ("bool", None),
    "debug_spinlock": ("bool", None),
    "debug_rwsems": ("bool", None),
    "kasan_mode": ("enum", ("hw_tags", "sw_tags", "generic", "off")),
    "kcsan": ("bool", None),
    "randstruct": ("enum", ("none", "full", "performance")),
    "page_size": ("enum", (4096, 16384, 65536)),
    "va_bits": ("enum", (39, 48)),
    "max_skb_frags": ("range", (1, 64)),
    "skb_extensions": ("bool", None),
    "net_ns": ("bool", None),
    "netfilter": ("bool", None),
    "ipv4": ("bool", None),
    "ipv6": ("bool", None),
    "nf_conntrack": ("bool", None),
    "nf_conntrack_mark": ("bool", None),
    "nf_conntrack_secmark": ("bool", None),
    "nf_conntrack_zones": ("bool", None),
    "nf_conntrack_labels": ("bool", None),
    "nf_tables": ("bool", None),
    "ip_vs": ("bool", None),
    "xfrm": ("bool", None),
    "wext_core": ("bool", None),
    "tmpfs_quota": ("bool", None),
    "fs_posix_acl": ("bool", None),
    "security": ("bool", None),
    "selinux": ("bool", None),
    "sysctl": ("bool", None),
    "memfd_create": ("bool", None),
    "keys": ("bool", None),
    "bsd_process_acct": ("bool", None),
    "cgroups": ("bool", None),
    "cgroup_subsys_count": ("range", (1, 16)),
    "cgroup_bpf": ("bool", None),
    "psi": ("bool", None),
    "cgroup_sched": ("bool", None),
    "memcg": ("bool", None),
    "mem_alloc_profiling": ("bool", None),
    "compat": ("bool", None),
    "modversions": ("bool", None),
    "cfi_clang": ("bool", None),
}

FLAGS = {
    "xtables_compat": {
        0: {"DNSK_XT_NOCOMPAT": "1", "DNSK_XT_LAYOUT": "104"},
        1: {"DNSK_XT_NOCOMPAT": "", "DNSK_XT_LAYOUT": "128"},
    },
}


def check_value(path: Path, key: str, value: object) -> int | str:
    spec, allowed = CONFIG_SPECS[key]
    if spec == "bool":
        if isinstance(value, bool):
            return int(value)
        if isinstance(value, int) and value in (0, 1):
            return value
        raise ValueError(f"{path}: {key} must be 0 or 1")
    if spec == "enum":
        if value not in allowed:
            raise ValueError(f"{path}: {key} must be one of {allowed}")
        return value
    if spec == "range":
        low, high = allowed
        if not isinstance(value, int) or isinstance(value, bool) or not low <= value <= high:
            raise ValueError(f"{path}: {key} must be an integer in [{low}, {high}]")
        return value
    raise AssertionError(spec)


def load(path: Path) -> tuple[str, str, dict[str, int | str]]:
    data = json.loads(path.read_text())
    if data.get("schema") != SUPPORTED_SCHEMA:
        raise ValueError(f"{path}: unsupported schema {data.get('schema')!r}")
    profile_id = data.get("id")
    kmi = data.get("kmi")
    config = data.get("config")
    if not isinstance(profile_id, str) or not profile_id:
        raise ValueError(f"{path}: missing id")
    if not isinstance(kmi, str) or not kmi:
        raise ValueError(f"{path}: missing kmi")
    if not isinstance(config, dict):
        raise ValueError(f"{path}: missing config object")

    unknown = sorted(set(config) - set(CONFIG_SPECS))
    if unknown:
        raise ValueError(f"{path}: unsupported config key {unknown[0]!r}")
    missing = sorted(set(CONFIG_SPECS) - set(config))
    if missing:
        raise ValueError(f"{path}: missing config key {missing[0]!r}")

    checked = {key: check_value(path, key, config[key]) for key in CONFIG_SPECS}
    return profile_id, kmi, checked


def flags_for(profile_id: str, config: dict[str, int | str]) -> dict[str, str]:
    flags = {"DNSK_ABI_PROFILE": profile_id}
    flags.update(FLAGS["xtables_compat"][int(config["xtables_compat"])])
    flags["DNSK_PAGE_SIZE"] = str(config["page_size"])
    flags["DNSK_VA_BITS"] = str(config["va_bits"])
    flags["DNSK_MAX_SKB_FRAGS"] = str(config["max_skb_frags"])
    flags["DNSK_CGROUP_SUBSYS_COUNT"] = str(config["cgroup_subsys_count"])
    return flags


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--kmi")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        profile_id, kmi, config = load(args.profile)
        if args.kmi and args.kmi != kmi:
            raise ValueError(
                f"{args.profile}: profile KMI {kmi!r} does not match build KMI {args.kmi!r}"
            )
        flags = flags_for(profile_id, config)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"build_flags: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps({"id": profile_id, "kmi": kmi, "flags": flags}, sort_keys=True))
    else:
        print(f"DNSK_BUILD_PROFILE={shlex.quote(profile_id)}")
        for key, value in sorted(flags.items()):
            print(f"{key}={shlex.quote(value)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
