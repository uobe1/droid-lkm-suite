#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a build ABI profile into build flags.

Profiles under profiles/abi/ describe kernel config choices that are not
frozen by the GKI KMI but change struct layouts used by our modules. The
emitted assignments are evaluated by the CI build step and passed to Kbuild.
"""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path

SUPPORTED_SCHEMA = 1
FLAGS = {
    "xtables_compat": {
        0: {"DNSK_XT_NOCOMPAT": "1", "DNSK_XT_LAYOUT": "104"},
        1: {"DNSK_XT_NOCOMPAT": "", "DNSK_XT_LAYOUT": "128"},
    },
}


def load(path: Path) -> tuple[str, dict[str, str]]:
    data = json.loads(path.read_text())
    if data.get("schema") != SUPPORTED_SCHEMA:
        raise ValueError(f"{path}: unsupported schema {data.get('schema')!r}")
    profile_id = data.get("id")
    kmi = data.get("kmi")
    abi = data.get("abi")
    if not isinstance(profile_id, str) or not profile_id:
        raise ValueError(f"{path}: missing id")
    if not isinstance(kmi, str) or not kmi:
        raise ValueError(f"{path}: missing kmi")
    if not isinstance(abi, dict):
        raise ValueError(f"{path}: missing abi object")
    flags: dict[str, str] = {}
    for key, value in abi.items():
        choices = FLAGS.get(key)
        if choices is None:
            raise ValueError(f"{path}: unsupported abi key {key!r}")
        if value not in choices:
            raise ValueError(f"{path}: unsupported {key}={value!r}")
        flags.update(choices[value])
    return profile_id, flags


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        profile_id, flags = load(args.profile)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"build_flags: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps({"id": profile_id, "flags": flags}, sort_keys=True))
    else:
        print(f"DNSK_BUILD_PROFILE={shlex.quote(profile_id)}")
        for key, value in sorted(flags.items()):
            print(f"{key}={shlex.quote(value)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
