#!/bin/sh
# Load / control / unload hook-bench.kpm through the APatch SuperCall protocol.
#
#   tests/kpm/env/load_kpm.sh load   [KPM_PATH]
#   tests/kpm/env/load_kpm.sh ctl0   [ARGS]
#   tests/kpm/env/load_kpm.sh unload
#
# The manager JNI is not needed: APatch ships the `kpatch` userspace CLI
# (lib/arm64-v8a/libkpatch.so in the APK, installed here as $KPATCH).  Its
# KPM command set, confirmed from the binary strings, is:
#   kpm load <PATH> [ARGS] / kpm unload <NAME> / kpm ctl0 <NAME> <ARGS>
#   kpm num / kpm list / kpm info <NAME>
#
# SuperCall wire format recovered from libkpatch.so disassembly (0.13.9 APK):
#   syscall(45, superkey_string_ptr,
#           (hash31(superkey) & 0xffff0000) | command,
#           arg1, arg2, 0)
#   command 0x1020 = KPM load, 0x1021 = unload, 0x1022 = ctl0.
# The superkey is stored in /data/adb/kpatch; `kpatch key set` writes it.
# Kernel side is kp_kpm_load_sc / kp_kpm_control_sc / kp_kpm_list_sc /
# kp_kpm_info_sc in android16-6.12_kernelpatch.ko.
#
# Not implemented here: reimplementing SuperCall in C.  The shipped CLI is
# byte-exact and avoids guessing the key-hash/auth edge cases.
set -eu

KPATCH=${KPATCH:-/data/local/tmp/kpm-bench/kpatch}
NAME=${NAME:-dnsk-hook-bench}
CMD=${1:?usage: load_kpm.sh load|ctl0|unload ...}
shift || true

test -x "$KPATCH" || { echo "load_kpm: no kpatch at $KPATCH" >&2; exit 1; }

# Optional: provision the superkey before the first call.  kpatch key set is a
# local file write; the kernel-side key must have been fixed at patch/late-load
# time (kptools -s or apd).
if [ -n "${KEY:-}" ]; then
    mkdir -p /data/adb
    "$KPATCH" key set "$KEY"
fi

case "$CMD" in
load)
    MOD=${1:-/data/local/tmp/kpm-bench/hook-bench.kpm}
    test -f "$MOD" || { echo "load_kpm: no $MOD" >&2; exit 1; }
    "$KPATCH" kpm load "$MOD" "${KPM_ARGS:-}"
    "$KPATCH" kpm num
    "$KPATCH" kpm list
    "$KPATCH" kpm info "$NAME"
    ;;
ctl0)
    "$KPATCH" kpm ctl0 "$NAME" "$@"
    ;;
unload)
    "$KPATCH" kpm unload "$NAME"
    "$KPATCH" kpm num
    ;;
*)
    echo "load_kpm: unknown command $CMD" >&2
    exit 1
    ;;
esac
