#!/bin/sh
# Headless KernelPatch runtime load on the real device via apd late-load.
#
#   tests/kpm/env/load_kp_late.sh            (run as root on beryl, adb shell su -c)
#
# Sequence (from docs/evidence/kpm-evaluation.md and the APatch apd strings):
#   1. apd relocates the KMI .ko itself against /proc/kallsyms and calls
#      init_module(2); it is not plain `insmod`.
#   2. It reads /data/adb/ap/{KMI}_kernelpatch.ko, so the extracted asset must
#      be pushed to that exact path.
#   3. It then applies the Magisk SELinux policy and forces enforce=1, which is
#      why the test-only selpermissive.ko must already be armed by ksud.
#
# Requirements (hard):
#   - root (KSU jailbreak), CONFIG_MODULES, CONFIG_KALLSYMS_ALL,
#     !MODULE_SIG_FORCE (the shipped .ko is unsigned, see extract manifest);
#   - SELinux permissive before late-load (ksud loads selpermissive.ko);
#   - the .ko vermagic must match `uname -r`; the asset says 6.12.76-4k-...,
#     beryl says 6.12.30-android16-5.  Pre-patch with patch_vermagic.py unless
#     apd is proven to rewrite it (no "vermagic" string exists in libapd.so).
#
# This script only runs commands; it is not executed by the repository setup.
set -eu

DIR=${DIR:-/data/local/tmp/kpm-bench}
KMI=${KMI:-android16-6.12}
KO=${KO:-/data/adb/ap/${KMI}_kernelpatch.ko}
APD=${APD:-/data/adb/ap/bin/apd}
PKG=${PKG:-me.bmax.apatch}

log() { echo "load_kp_late: $*"; }

test "$(id -u)" = 0 || { log "must run as root"; exit 1; }
test -x "$APD" || { log "no apd at $APD"; exit 1; }
test -f "$KO" || { log "no $KO; push the extracted runtime there first"; exit 1; }

if [ -d /sys/module/kernelpatch ]; then
    log "kernelpatch already loaded; skipping late-load"
else
    # Exact apd form documented for APatch 11282.  Keep --magica unset: it
    # opens an adb root port and is not needed here.
    "$APD" late-load --module "$KO" --kmi "$KMI" --package-name "$PKG"
fi

test -d /sys/module/kernelpatch || { log "late-load did not register kernelpatch"; exit 1; }
log "runtime loaded: $(cat /sys/module/kernelpatch/version 2>/dev/null || echo '?')"

# KernelPatch supersedes the magic syscall 45; use the bundled kpatch CLI to
# prove the superkey works before any KPM operation.
KPATCH=${KPATCH:-$DIR/kpatch}
KEY=${KEY:-}
if [ -n "$KEY" ] && [ -x "$KPATCH" ]; then
    mkdir -p /data/adb
    "$KPATCH" key set "$KEY"
fi
if [ -x "$KPATCH" ]; then
    "$KPATCH" kpver || { log "kpatch kpver failed: superkey mismatch or no SuperCall"; exit 1; }
fi
log "ready"
