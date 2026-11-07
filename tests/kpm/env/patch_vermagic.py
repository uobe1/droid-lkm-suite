#!/usr/bin/env python3
"""Patch a KernelPatch runtime .ko for a kernel whose exact vermagic differs.

The APK ships android16-6.12_kernelpatch.ko with
    vermagic=6.12.76-4k-gae4e2f4f997e-dirty SMP preempt mod_unload modversions aarch64
while beryl reports 6.12.30-android16-5.  A plain init_module(2) rejects that;
apd's late-load path does not rewrite vermagic itself (no such string in
libapd.so), so rewrite it here before pushing the .ko.

    patch_vermagic.py KO --vermagic "6.12.30-android16-5 SMP preempt mod_unload modversions aarch64" \
        [--module-layout-crc 0xe976b219] [--out KO.patched]

The replacement value is written in place (NUL padded), so it must be no
longer than the original field.  __versions is patched only when a
module_layout entry already exists; adding one needs an ELF rebuild and is
reported as unsupported instead of being guessed.

This is an offline file transform.  It never loads a module.
"""
import argparse
import struct
import sys

MODINFO = b".modinfo"
VERSIONS = b"__versions"


def sections(data):
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit("patch_vermagic: not a little-endian ELF64 file")
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    shnum = struct.unpack_from("<H", data, 0x3C)[0]
    shstrndx = struct.unpack_from("<H", data, 0x3E)[0]
    if shoff == 0 or shnum == 0:
        raise SystemExit("patch_vermagic: no section table")

    def sh(i):
        off = shoff + i * shentsize
        name, typ = struct.unpack_from("<II", data, off)
        flags, addr, offset, size = struct.unpack_from("<QQQQ", data, off + 8)
        return {"name_off": name, "type": typ, "flags": flags, "addr": addr,
                "offset": offset, "size": size}

    headers = [sh(i) for i in range(shnum)]
    strtab = headers[shstrndx]
    strdata = data[strtab["offset"]:strtab["offset"] + strtab["size"]]

    def name(h):
        end = strdata.find(b"\0", h["name_off"])
        return strdata[h["name_off"]:end if end >= 0 else None]

    for h in headers:
        h["sname"] = name(h)
    return headers


def patch_modinfo(data, headers, vermagic):
    for h in headers:
        if h["sname"] == MODINFO:
            start, size = h["offset"], h["size"]
            blob = data[start:start + size]
            tag = b"vermagic="
            i = blob.find(tag)
            if i < 0:
                raise SystemExit("patch_vermagic: no vermagic= in .modinfo")
            end = blob.find(b"\0", i)
            if end < 0:
                raise SystemExit("patch_vermagic: unterminated vermagic")
            old = blob[i + len(tag):end]
            new = vermagic.encode() if isinstance(vermagic, str) else vermagic
            if len(new) > len(old):
                raise SystemExit(
                    "patch_vermagic: new vermagic is longer (%d > %d bytes); "
                    "refusing to grow .modinfo" % (len(new), len(old)))
            repl = tag + new + b"\0" * (end - i - len(tag) - len(new))
            data[start + i:start + i + len(repl)] = repl
            return old.decode("utf-8", "replace")
    raise SystemExit("patch_vermagic: .modinfo section not found")


def patch_module_layout(data, headers, crc, width):
    for h in headers:
        if h["sname"] == VERSIONS:
            start, size = h["offset"], h["size"]
            blob = data[start:start + size]
            stride = 64
            for off in range(0, len(blob) - stride + 1, stride):
                entry = blob[off:off + stride]
                if entry[width:stride].split(b"\0")[0] == b"module_layout":
                    struct.pack_into("<I" if width == 4 else "<Q",
                                     data, start + off, crc)
                    return True
            return False
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ko")
    ap.add_argument("--vermagic", required=True)
    ap.add_argument("--module-layout-crc", type=lambda s: int(s, 0), default=None,
                    help="e.g. 0xe976b219; written into an existing __versions entry")
    ap.add_argument("--crc-width", type=int, choices=(4, 8), default=8,
                    help="8 for arm64 GKI (default), 4 for CONFIG_MODULE_REL_CRCS")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    with open(args.ko, "rb") as f:
        data = bytearray(f.read())
    headers = sections(data)
    old = patch_modinfo(data, headers, args.vermagic)
    print("vermagic: %r -> %r" % (old, args.vermagic))

    if args.module_layout_crc is not None:
        if patch_module_layout(data, headers, args.module_layout_crc, args.crc_width):
            print("module_layout crc -> 0x%x (width %d)"
                  % (args.module_layout_crc, args.crc_width))
        else:
            raise SystemExit(
                "patch_vermagic: no existing module_layout entry in __versions; "
                "adding one requires an ELF rebuild (unsupported).  Omit "
                "--module-layout-crc and let apd/insmod prove the requirement.")

    out = args.out or args.ko
    with open(out, "wb") as f:
        f.write(data)
    print("wrote %s" % out)


if __name__ == "__main__":
    sys.exit(main())
