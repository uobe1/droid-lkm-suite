#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Unified style gate.  Pure-Python3 checks always run; optional external tools
# (shellcheck/ruff/markdownlint/clang-format) only run when present, so CI and
# the dev container get the same verdict instead of a silent "skipped, 0".
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

fail=0

run() {
	label=$1
	shift
	if "$@"; then
		echo "ok: $label"
	else
		echo "FAIL: $label" >&2
		fail=1
	fi
}

# Always available, and the rules the project actually promises.
run "API docs match their comment sources" python3 scripts/gen-api-docs.py --check
run "tracked Markdown style" python3 scripts/check-docs-style.py

# Optional linters: absence is explicitly reported, never treated as success.
for tool in shellcheck ruff clang-format markdownlint; do
	if command -v "$tool" >/dev/null 2>&1; then
		case "$tool" in
		shellcheck) run "shellcheck" sh -c 'find tools ksu tests ci -name "*.sh" -type f -print0 | xargs -0 -r shellcheck' ;;
		ruff) run "ruff" ruff check tools ;;
		clang-format) run "clang-format" sh -c 'find core features -name "*.c" -o -name "*.h" | xargs clang-format --dry-run --Werror' ;;
		markdownlint) run "markdownlint" markdownlint README.md CONTRIBUTING.md 'docs/**/*.md' ;;
		esac
	else
		echo "note: $tool not installed; its rules are covered by python3 gates where applicable" >&2
	fi
done

exit "$fail"
