#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

fail=0

if command -v shellcheck >/dev/null 2>&1; then
	find tools ksu tests ci -name '*.sh' -type f -print0 2>/dev/null |
		xargs -0 -r shellcheck
else
	echo "shellcheck not installed; skipping" >&2
fi

if command -v ruff >/dev/null 2>&1; then
	ruff check tools
else
	echo "ruff not installed; skipping" >&2
fi

if command -v markdownlint >/dev/null 2>&1; then
	markdownlint 'README.md' 'CONTRIBUTING.md' 'docs/**/*.md'
else
	echo "markdownlint not installed; skipping" >&2
fi

exit "$fail"
