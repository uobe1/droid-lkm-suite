#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate per-KMI profile scaffolding.

The profile is intentionally conservative. Native capability entries describe
what the upstream KMI can provide when built with the relevant config; they are
not validation results. `validated` remains false until CI/device evidence is
attached by the release gate.
"""

from __future__ import annotations

import json
from pathlib import Path

KMIS = [
    "android12-5.10",
    "android13-5.10",
    "android13-5.15",
    "android14-5.15",
    "android14-6.1",
    "android15-6.6",
    "android16-6.12",
    "android17-6.18",
]

FEATURES = [
    "core",
    "ns",
    "devtmpfs",
    "netfilter",
    "ipset",
    "cgpids",
    "userns",
    "tmpfsx",
]


def major_minor(kmi: str) -> tuple[int, int]:
    version = kmi.split("-", 1)[1]
    major, minor = version.split(".")
    return int(major), int(minor)


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    out = root / "profiles"
    out.mkdir(exist_ok=True)
    for kmi in KMIS:
        major, minor = major_minor(kmi)
        idmapped = "supported" if (major, minor) >= (5, 12) else "n/a"
        pidfs = "supported" if (major, minor) >= (6, 12) else "n/a"
        dev_ns = kmi == "android16-6.12"
        profile = {
            "schema": 1,
            "kmi": kmi,
            "validated": False,
            "stage": "M1-ns-development" if dev_ns else "M0-core-scaffold",
            "features": {
                name: {
                    "status": (
                        "scaffold" if name == "core" else
                        "development" if dev_ns and name in ("ns", "devtmpfs") else
                        "not-shipped"
                    ),
                    "target_level": "n/a" if name == "core" else {
                        "ns": "R",
                        "devtmpfs": "R",
                        "netfilter": "R",
                        "ipset": "R",
                        "cgpids": "V/D",
                        "userns": "D/N",
                        "tmpfsx": "D/N",
                    }[name],
                }
                for name in FEATURES
            },
            "native_capability": {
                "idmapped_mount": idmapped,
                "pidfs": pidfs,
                "user_ns": "config-dependent",
                "tmpfs_xattr": "config-dependent",
                "tmpfs_posix_acl": "config-dependent",
                "cgroup_pids": "config-dependent",
                "nf_tables": "config-dependent",
                "ip_set": "config-dependent",
                "cfs_bandwidth": "config-dependent",
            },
            "notes": [
                "Native capability is a KMI/upstream property, not a test result.",
                "Feature status stays not-shipped until its acceptance evidence exists.",
            ],
        }
        path = out / f"{kmi}.json"
        path.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n")
        print(path.relative_to(root))


if __name__ == "__main__":
    main()
