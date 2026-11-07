#!/bin/sh
# Extract the KernelPatch runtime/tools from an APatch manager APK.
#
#   tests/kpm/env/extract_apk.sh /path/APatch.apk [OUT_DIR]
#
# No install, no execution: python3 zipfile + readelf/strings only.  The
# output directory defaults to tests/kpm/env/out and is gitignored; the
# binaries must never be committed.
set -eu

APK=${1:?usage: extract_apk.sh APK [OUT_DIR]}
OUT=${2:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/out}

test -f "$APK" || { echo "extract_apk: no such APK: $APK" >&2; exit 1; }
command -v python3 >/dev/null || { echo "extract_apk: python3 required" >&2; exit 1; }

mkdir -p "$OUT"

python3 - "$APK" "$OUT" <<'PY'
import hashlib
import os
import stat
import struct
import sys
import zipfile

apk, out = sys.argv[1], sys.argv[2]

# APK entry -> flat output name.  libapd.so and libkptools.so are PIE
# executables (not shared libraries) despite the lib prefix; libkpatch.so is
# the kpatch userspace CLI.  Only this KMI's KernelPatch runtime is needed.
WANT = {
    "assets/android16-6.12_kernelpatch.ko": "android16-6.12_kernelpatch.ko",
    "lib/arm64-v8a/libapd.so": "libapd.so",
    "lib/arm64-v8a/libkpatch.so": "kpatch",
    "lib/arm64-v8a/libkptools.so": "libkptools.so",
    "assets/kpimg": "kpimg",
    "assets/main.jar": "main.jar",
    "assets/InstallAP.sh": "InstallAP.sh",
    "assets/boot_patch.sh": "boot_patch.sh",
}
EXECUTABLE = {"libapd.so", "kpatch", "libkptools.so", "InstallAP.sh", "boot_patch.sh"}

z = zipfile.ZipFile(apk)
names = set(z.namelist())
missing = [n for n in WANT if n not in names]
if missing:
    raise SystemExit("extract_apk: missing APK entries: %s" % ", ".join(missing))

manifest = []
for entry, name in WANT.items():
    data = z.read(entry)
    path = os.path.join(out, name)
    with open(path, "wb") as f:
        f.write(data)
    if name in EXECUTABLE:
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    manifest.append((name, len(data), hashlib.sha256(data).hexdigest()))

# Static facts used by the bench scripts and README: KP ABI version and the
# vermagic baked into the runtime .ko.
ko = open(os.path.join(out, "android16-6.12_kernelpatch.ko"), "rb").read()
def field(tag):
    # The module also embeds printf format strings such as "version=%s";
    # only a NUL-terminated value that is not a bare %s is real modinfo.
    pos = 0
    while True:
        i = ko.find(tag, pos)
        if i < 0:
            return "unknown"
        j = ko.find(b"\0", i)
        val = ko[i + len(tag):j]
        if val and b"%s" not in val:
            return val.decode("utf-8", "replace")
        pos = i + 1
kp_version = field(b"version=")
vermagic = field(b"vermagic=")
# ELF section names are NUL-terminated; a .sig section would mean the module
# is signed.  Report presence explicitly.
has_sig = b".sig" in ko or b"~Module signature appended~" in ko

lines = []
for name, size, digest in manifest:
    lines.append("%s %d %s" % (name, size, digest))
lines.append("kernelpatch_version=%s" % kp_version)
lines.append("kernelpatch_vermagic=%s" % vermagic)
lines.append("kernelpatch_signed=%s" % ("yes" if has_sig else "no"))
with open(os.path.join(out, "manifest.txt"), "w") as f:
    f.write("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo "extract_apk: output in $OUT"
