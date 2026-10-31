#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Import guard: reject runtime-resolver imports and forbidden symbol prefixes.

This is deliberately narrow. It does not pretend to prove that every remaining
undefined symbol is exported; the kernel build's modpost is the authority for
that. It catches the specific mistakes that make a module depend on KernelSU's
kallsyms_* exports or on CFI/ksymtab internals.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

FORBIDDEN_PREFIXES = ("kallsyms_", "__cfi_", "__ksymtab_", "__kcrctab_")
FORBIDDEN_EXACT = {
    "kallsyms_lookup_name",
    "kallsyms_on_each_symbol",
    "kallsyms_lookup",
    "kallsyms_lookup_size_offset",
}


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
        if parts[0] == "U" and len(parts) >= 2:
            names.append(parts[-1])
        elif "UND" in parts:
            names.append(parts[-1])
    return sorted(set(names))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ko", type=Path)
    parser.add_argument("--allow", type=Path, help="optional allowlist, one symbol per line")
    args = parser.parse_args()

    if not args.ko.is_file():
        print(f"missing module: {args.ko}", file=sys.stderr)
        return 2
    allow = set()
    if args.allow and args.allow.is_file():
        allow = {
            line.strip()
            for line in args.allow.read_text().splitlines()
            if line.strip() and not line.startswith("#")
        }

    bad = []
    for name in undefined_symbols(args.ko):
        if name in allow:
            continue
        if name in FORBIDDEN_EXACT or name.startswith(FORBIDDEN_PREFIXES):
            bad.append(name)

    if bad:
        print("forbidden undefined symbols:", file=sys.stderr)
        for name in bad:
            print(f"  {name}", file=sys.stderr)
        return 1
    print(f"imports guard ok: {args.ko}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
