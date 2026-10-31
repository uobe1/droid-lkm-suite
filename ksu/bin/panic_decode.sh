#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Extract structured DNSK panic markers from ramoops/pstore or last_kmsg.
set -eu

found=0
scan() {
	[ -r "$1" ] || return 0
	if grep -E 'DNSK_PANIC|dnsk:' "$1" >/dev/null 2>&1; then
		echo "== $1 =="
		grep -E 'DNSK_PANIC|dnsk:' "$1" | tail -200
		found=1
	fi
}

scan /proc/last_kmsg
for f in /sys/fs/pstore/console-ramoops* /sys/fs/pstore/dmesg-ramoops*; do
	[ -e "$f" ] || continue
	scan "$f"
done

if [ "$found" -eq 0 ]; then
	echo "no DNSK panic markers found"
	exit 1
fi
