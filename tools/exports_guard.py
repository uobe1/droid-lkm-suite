#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exported-symbol guard for out-of-tree modules.

Every undefined symbol in a .ko must either be listed in the kernel's
Module.symvers/vmlinux.symvers or start with dnsk_ (core API resolved at
runtime). Anything else would make the module depend on a non-exported kernel
symbol and fail to load on a stock GKI kernel, even though modpost only warns.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

DEFAULT_ALLOW_PREFIXES = ("dnsk_",)


def undefined_symbols(ko: Path) -> list[str]:
    try:
        out = subprocess.check_output(["nm", "-u", str(ko)], text=True)
    except FileNotFoundError:
        out = subprocess.check_output(["readelf", "-Ws", str(ko)], text=True)
    names: list[str] = []
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        if (parts[0] == "U" and len(parts) >= 2) or "UND" in parts:
            names.append(parts[-1])
    return sorted(set(names))


def exported_symbols(symvers: Path) -> set[str]:
    symbols: set[str] = set()
    for line in symvers.read_text(errors="replace").splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith("0x"):
            symbols.add(parts[1])
    return symbols


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--symvers", type=Path, required=True)
    parser.add_argument("--allow-prefix", action="append", default=[])
    parser.add_argument("--allow", type=Path, help="optional exact-name allowlist")
    parser.add_argument("ko", type=Path, nargs="+")
    args = parser.parse_args()

    if not args.symvers.is_file():
        print(f"missing symvers: {args.symvers}", file=sys.stderr)
        return 2
    allowed = set(DEFAULT_ALLOW_PREFIXES) | set(args.allow_prefix)
    if args.allow and args.allow.is_file():
        allowed |= {
            line.strip()
            for line in args.allow.read_text().splitlines()
            if line.strip() and not line.startswith("#")
        }
    exported = exported_symbols(args.symvers)
    failures: list[tuple[Path, str]] = []
    for ko in args.ko:
        if not ko.is_file():
            print(f"missing module: {ko}", file=sys.stderr)
            return 2
        for name in undefined_symbols(ko):
            if name in exported or name.startswith(tuple(allowed)):
                continue
            failures.append((ko, name))

    if failures:
        print("undefined symbols not exported by the kernel:", file=sys.stderr)
        for ko, name in failures:
            print(f"  {ko.name}: {name}", file=sys.stderr)
        return 1
    print(f"exports guard ok: {len(args.ko)} module(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
