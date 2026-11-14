#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure-Python3 style gate for tracked Markdown and documentation comments.

The dev container has no markdownlint/clang-format/shellcheck, and the hard
constraint forbids installing them.  This script encodes the rules that
docs/STYLE.md actually promises, so `make check` fails on a real violation
instead of printing "skipped" and exiting 0.

Rules enforced over tracked *.md (excluding gitignored notes/evidence/plans):

- no fullwidth punctuation, no fullwidth space
- no trailing whitespace
- fenced code blocks declare a language
- ATX headings do not skip levels
- local links resolve to an existing file
- one sentence per line is reported as a warning, not an error

Usage:
    check-docs-style.py [--strict]

Exit codes: 0 clean, 1 violations, 2 usage error.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FULLWIDTH = set('，。；：？！（）「」『』【】《》＃＄％＆＊＋－／＝＠＼＾＿｀｜～￥')
FULLWIDTH_SPACE = '\u3000'
FENCE = re.compile(r'^\s*```(\S*)\s*$')
HEADING = re.compile(r'^(#{1,6})\s+(.*)$')
LINK = re.compile(r'\[[^\]]*\]\(([^)]+)\)')
GITIGNORED_DIRS = ('docs/notes', 'docs/evidence', 'docs/plans')
# AGENTS.md is itself gitignored local working material, not a published page.
LOCAL_ONLY = {'AGENTS.md'}


def md_files():
    """Only tracked Markdown is a published page the style rules govern."""
    try:
        tracked = subprocess.check_output(
            ['git', 'ls-files', '*.md'], cwd=ROOT, text=True
        ).splitlines()
    except (subprocess.CalledProcessError, FileNotFoundError):
        tracked = [p.relative_to(ROOT).as_posix()
                   for p in ROOT.rglob('*.md')]
    seen = set()
    for rel in sorted(tracked):
        if rel in LOCAL_ONLY or rel.startswith(GITIGNORED_DIRS) or rel.startswith('build/'):
            continue
        seen.add(rel)
        yield ROOT / rel, rel
    # Generated API pages are tracked deliverables too, but are not yet in the
    # index when a fresh clone runs `git ls-files` on a dirty worktree.
    for rel in ('docs/api/kernel.md', 'docs/api/userspace.md'):
        if rel not in seen and (ROOT / rel).exists():
            yield ROOT / rel, rel


def check_file(path, rel, strict):
    problems = []
    warnings = []
    text = path.read_text(encoding='utf-8')
    lines = text.splitlines()
    level = 0
    in_fence = False
    for i, line in enumerate(lines, 1):
        fm = FENCE.match(line)
        if fm:
            if not in_fence:
                if fm.group(1) == '':
                    problems.append(f'{rel}:{i}: fenced code block without language')
            else:
                if fm.group(1) != '':
                    problems.append(
                        f'{rel}:{i}: closing fence carries language {fm.group(1)!r}')
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        if line.rstrip() != line:
            problems.append(f'{rel}:{i}: trailing whitespace')
        for ch in line:
            if ch == FULLWIDTH_SPACE:
                problems.append(f'{rel}:{i}: fullwidth space')
            elif ch in FULLWIDTH:
                problems.append(f'{rel}:{i}: fullwidth punctuation {ch!r}')
        hm = HEADING.match(line)
        if hm:
            new = len(hm.group(1))
            if new > level + 1:
                problems.append(f'{rel}:{i}: heading jumps from h{level} to h{new}')
            level = new
        for m in LINK.finditer(line):
            target = m.group(1).split('#')[0]
            if not target or '://' in target or target.startswith('mailto:'):
                continue
            resolved = (path.parent / target).resolve()
            if not resolved.exists():
                problems.append(f'{rel}:{i}: broken local link -> {target}')
        # A line with two ASCII sentence terminators followed by a space is a
        # strong signal of two sentences on one line (STYLE.md asks for one).
        if strict and re.search(r'[.;:]\s+[A-Za-z\u4e00-\u9fff]', line):
            warnings.append(f'{rel}:{i}: possible two sentences on one line')
    return problems, warnings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--strict', action='store_true', help='also report one-line-two-sentence warnings')
    args = ap.parse_args()
    problems, warnings = [], []
    for path, rel in md_files():
        p, w = check_file(path, rel, args.strict)
        problems.extend(p)
        warnings.extend(w)
    for w in warnings:
        print(f'warning: {w}', file=sys.stderr)
    for p in problems:
        print(f'error: {p}', file=sys.stderr)
    if problems:
        print(f'check-docs-style: {len(problems)} violation(s)', file=sys.stderr)
        return 1
    print(f'check-docs-style: clean ({len(list(md_files()))} tracked md files)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
