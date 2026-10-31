#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""KMI matrix used by CI and packaging. No network access."""

from __future__ import annotations

import argparse
import json
import sys

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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", action="store_true", help="GitHub Actions matrix JSON")
    parser.add_argument("--check", action="store_true", help="validate the KMI list")
    parser.add_argument("--list", action="store_true", help="print one KMI per line")
    parser.add_argument("--ddk-image", metavar="KMI", help="print the DDK image for a KMI")
    args = parser.parse_args()

    if args.check:
        if len(KMIS) != len(set(KMIS)) or len(KMIS) != 8:
            print("KMI matrix is invalid", file=sys.stderr)
            return 1
        print("KMI matrix ok: " + ", ".join(KMIS))
        return 0
    if args.ddk_image:
        if args.ddk_image not in KMIS:
            print(f"unknown KMI: {args.ddk_image}", file=sys.stderr)
            return 2
        print(f"ghcr.io/ylarod/ddk-min:{args.ddk_image}-20260828")
        return 0
    if args.list:
        print("\n".join(KMIS))
        return 0
    if args.json:
        print(json.dumps({"kmi": KMIS}))
        return 0
    print("\n".join(KMIS))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
