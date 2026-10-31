#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a KernelSU module ZIP from built artifacts.

This script never invents modules.  A requested KMI must have a built
dnsk_core.ko.  Feature modules are included only when their .ko exists; the
manifest records exactly what was packed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kmi_matrix import KMIS  # noqa: E402


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_commit(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return "unknown"


def add_file(zf: zipfile.ZipFile, src: Path, arc: str, mode: int) -> None:
    info = zipfile.ZipInfo(arc)
    info.external_attr = (mode & 0xFFFF) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    zf.writestr(info, src.read_bytes())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--module-root", type=Path, default=None)
    parser.add_argument("--build-root", type=Path, default=None)
    parser.add_argument("--bin-root", type=Path, default=None)
    parser.add_argument("--kmi", action="append", default=[])
    parser.add_argument("--out", type=Path, default=Path("out/droid_lkm_suite-dev.zip"))
    parser.add_argument("--dev", action="store_true", help="mark this as a dev artifact; required")
    args = parser.parse_args()

    if not args.dev:
        print("refusing to package without --dev (release gate not implemented)", file=sys.stderr)
        return 2
    if not args.kmi:
        print("at least one --kmi is required", file=sys.stderr)
        return 2
    unknown = [k for k in args.kmi if k not in KMIS]
    if unknown:
        print("unknown KMI: " + ", ".join(unknown), file=sys.stderr)
        return 2

    root = args.root.resolve()
    module_root = (args.module_root or root / "ksu").resolve()
    build_root = (args.build_root or root / "build" / "kmi").resolve()
    bin_root = (args.bin_root or root / "build" / "bin").resolve()

    core_files: dict[str, Path] = {}
    feature_files: dict[str, dict[str, Path]] = {}
    for kmi in args.kmi:
        core = build_root / kmi / "dnsk_core.ko"
        if not core.is_file():
            print(f"missing core module for {kmi}: {core}", file=sys.stderr)
            return 1
        core_files[kmi] = core
        found_features: dict[str, Path] = {}
        for ko in sorted((build_root / kmi).glob("dnsk_*.ko")):
            if ko.name == "dnsk_core.ko":
                continue
            found_features[ko.stem] = ko
        feature_files[kmi] = found_features

    dnskctl = (root / "tools" / "dnskctl").resolve()
    preflight = (bin_root / "dnsk-preflight").resolve()
    panic_decode = (module_root / "bin" / "panic_decode.sh").resolve()
    for required in (dnskctl, preflight, panic_decode):
        if not required.is_file():
            print(f"missing packaging input: {required}", file=sys.stderr)
            return 1

    artifacts: list[dict[str, object]] = []

    def record(path: Path, arc: str) -> None:
        artifacts.append({"path": arc, "size": path.stat().st_size, "sha256": sha256(path)})

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as zf:
        for name in ("module.prop", "customize.sh", "uninstall.sh", "post-fs-data.sh",
                     "service.sh", "boot-completed.sh", "action.sh", "supported_kmis.txt",
                     "policy.conf"):
            src = module_root / name
            if not src.is_file():
                print(f"missing module file: {src}", file=sys.stderr)
                return 1
            mode = 0o755 if name.endswith(".sh") else 0o644
            add_file(zf, src, name, mode)
            record(src, name)

        webroot = module_root / "webroot"
        for src in sorted(webroot.rglob("*")):
            if src.is_file():
                arc = str(src.relative_to(module_root))
                add_file(zf, src, arc, 0o644)
                record(src, arc)

        for src, arc, mode in (
            (dnskctl, "bin/dnskctl", 0o755),
            (preflight, "bin/dnsk-preflight", 0o755),
            (panic_decode, "bin/panic_decode.sh", 0o755),
        ):
            add_file(zf, src, arc, mode)
            record(src, arc)

        for kmi, core in core_files.items():
            arc = f"kmi/{kmi}/{core.name}"
            add_file(zf, core, arc, 0o644)
            record(core, arc)
            for stem, ko in feature_files[kmi].items():
                arc = f"kmi/{kmi}/{ko.name}"
                add_file(zf, ko, arc, 0o644)
                record(ko, arc)

        manifest = {
            "schema": 1,
            "dev": True,
            "tagged": False,
            "status": "M0 core scaffold; feature modules are not shipped until acceptance gates pass",
            "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "source_commit": git_commit(root),
            "kmis": args.kmi,
            "features": {
                kmi: ["core"] + sorted(feature_files[kmi].keys()) for kmi in args.kmi
            },
            "artifacts": artifacts,
        }
        zf.writestr("manifest.json", json.dumps(manifest, indent=2, sort_keys=True))
        zf.writestr("verification.json", json.dumps({
            "dev": True,
            "tagged": False,
            "note": "Dev artifact. No release/tag before all 8 KMI and all feature gates pass.",
        }, indent=2, sort_keys=True))

    print(f"package: {args.out}")
    print(f"sha256: {sha256(args.out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
