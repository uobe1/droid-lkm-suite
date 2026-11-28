#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Unified local style gate.  Pure-Python3 checks always run.  Optional external
# tools run only when installed; when a tool is missing the gate prints
# "SKIPPED: <tool> missing" and never claims the rule was checked.  CI enforces
# shellcheck, ruff and clang-format in .github/workflows/docs-check.yml.
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

# Every tracked shell file, including extensionless scripts such as
# tests/qemu/init* and tools/dnskctl.  Tracked filenames contain no spaces, so
# the newline-separated list is safe for xargs.
tracked_shell() {
	git ls-files '*.sh'
	git ls-files | while IFS= read -r f; do
		case "$f" in
		*.sh) continue ;;
		esac
		if head -n1 -- "$f" 2>/dev/null | grep -qE '^#!.*/(sh|bash)([[:space:]]|$)'; then
			printf '%s\n' "$f"
		fi
	done
}

lint_shellcheck() {
	tracked_shell | xargs -r shellcheck
}

lint_ruff() {
	ruff check scripts tools tests
}

# Project C/H only: vendored kernel sources and copied kselftests carry their
# own formatting and are not ours to reformat.
lint_clang_format() {
	git ls-files -z '*.c' '*.h' |
		grep -zEv '(^|/)(vendor|selftests)/' |
		xargs -0 -r clang-format --dry-run --Werror
}

# Always available, and the rules the project actually promises.
# API drift is checked once by `make docs-check` (and docs-check.yml in CI);
# do not repeat it here, that would be a second run of the same gate.
run "tracked Markdown style" python3 scripts/check-docs-style.py

# Optional linters: absent tools are reported as skipped, never as passed.
for tool in shellcheck ruff clang-format markdownlint; do
	if command -v "$tool" >/dev/null 2>&1; then
		case "$tool" in
		shellcheck) run "shellcheck" lint_shellcheck ;;
		ruff) run "ruff" lint_ruff ;;
		clang-format) run "clang-format" lint_clang_format ;;
		markdownlint) run "markdownlint" markdownlint README.md CONTRIBUTING.md 'docs/**/*.md' ;;
		esac
	else
		echo "SKIPPED: $tool missing; not checked in this run" >&2
	fi
done

exit "$fail"
