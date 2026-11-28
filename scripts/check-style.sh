#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# Unified local lint gate mirroring the lint job of
# .github/workflows/docs-check.yml, plus the tracked Markdown style check that
# the docs job runs:
#
#   tools: shellcheck, ruff, clang-format, yamllint, actionlint, cppcheck
#
# A tool is resolved from PATH, then uvx (ruff / clang-format / yamllint), then
# $DNSK_LINT_TOOLS (shellcheck / actionlint / cppcheck).  clang-format is
# pinned to the CI version (18.1.8); a PATH clang-format of another version is
# not used.  A surface whose tool cannot be found prints UNAVAILABLE and makes
# the gate exit 2 (incomplete); it is never counted as passed.
#
# Exit status: 0 = every surface ran and passed; 1 = a surface failed;
# 2 = a surface could not run because its tool is missing.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
LINT_TOOLS=${DNSK_LINT_TOOLS:-/tmp/lint-tools}
fail=0
incomplete=0

have_tool() {
	command -v "$1" >/dev/null 2>&1
}

ok() { echo "ok: $1"; }
bad() {
	echo "FAIL: $1" >&2
	fail=1
}
unavailable() {
	echo "UNAVAILABLE: $1 (tool missing; surface not checked)" >&2
	incomplete=1
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

do_docs() {
	if python3 scripts/check-docs-style.py; then ok "tracked Markdown style"; else bad "tracked Markdown style"; fi
}

do_shellcheck() {
	if have_tool shellcheck; then
		sc=shellcheck
	elif [ -x "$LINT_TOOLS/shellcheck" ]; then
		sc="$LINT_TOOLS/shellcheck"
	else
		unavailable shellcheck
		return 0
	fi
	if tracked_shell | xargs -r "$sc"; then ok shellcheck; else bad shellcheck; fi
}

do_ruff() {
	if have_tool ruff; then
		set -- ruff
	elif have_tool uvx; then
		set -- uvx ruff
	else
		unavailable ruff
		return 0
	fi
	if "$@" check scripts tools tests; then ok ruff; else bad ruff; fi
}

# Project C/H only: vendored kernel sources and copied kselftests carry their
# own formatting and are not ours to reformat.
do_clang_format() {
	if have_tool uvx; then
		set -- uvx --from clang-format==18.1.8 clang-format
	elif have_tool clang-format && clang-format --version 2>/dev/null | grep -q 'version 18\.1\.8'; then
		set -- clang-format
	else
		unavailable clang-format 18.1.8
		return 0
	fi
	if git ls-files -z '*.c' '*.h' | grep -zEv '(^|/)(vendor|selftests)/' |
		xargs -0 -r "$@" --dry-run --Werror; then
		ok clang-format
	else
		bad clang-format
	fi
}

do_yamllint() {
	if have_tool yamllint; then
		set -- yamllint
	elif have_tool uvx; then
		set -- uvx yamllint
	else
		unavailable yamllint
		return 0
	fi
	if "$@" .github/workflows; then ok yamllint; else bad yamllint; fi
}

do_actionlint() {
	# actionlint runs shellcheck on embedded scripts, so expose our shellcheck.
	if have_tool actionlint; then
		set -- actionlint
	elif [ -x "$LINT_TOOLS/actionlint" ]; then
		set -- "$LINT_TOOLS/actionlint"
	else
		unavailable actionlint
		return 0
	fi
	if PATH="$LINT_TOOLS:$PATH" "$@"; then ok actionlint; else bad actionlint; fi
}

do_cppcheck() {
	if ! have_tool cppcheck; then
		unavailable cppcheck
		return 0
	fi
	if cppcheck --enable=warning,performance,portability --error-exitcode=1 \
		--std=c11 --suppress=missingIncludeSystem -I core/include \
		tools/dnsk_preflight.c; then
		ok cppcheck
	else
		bad cppcheck
	fi
}

do_docs
do_shellcheck
do_ruff
do_clang_format
do_yamllint
do_actionlint
do_cppcheck

if [ "$fail" -ne 0 ]; then
	exit 1
fi
if [ "$incomplete" -ne 0 ]; then
	exit 2
fi
exit 0
