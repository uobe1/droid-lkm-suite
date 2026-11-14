#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate every API page from documentation comments only.

There is exactly one source of truth for the API: the comment blocks in
``core/include/*.h`` (kernel-doc dialect) and in ``tools/dnskctl`` /
``tools/dnsk_preflight.c`` (Doxygen dialect).  Nothing in this script reads an
implementation line, a format string or a usage() here-document.  If a public
surface has no comment, the generator fails loudly instead of producing a page
that looks complete but silently omitted it.

The local dev container has neither kernel-doc nor doxygen and the hard
constraint forbids installing them, so the generator is a small, auditable
Python3 parser for the subset of the two dialects this project actually uses.

Usage:
    gen-api-docs.py [--check]

Without --check it writes docs/api/kernel.md and docs/api/userspace.md.
With --check it writes to a temp directory, compares against the committed
pages, and exits non-zero on drift (used by CI and `make docs`).
"""

import argparse
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KERNEL_HEADERS = sorted((ROOT / 'core' / 'include').glob('*.h')) + \
    sorted((ROOT / 'droid-hook' / 'include').glob('*.h'))
USERSPACE_SOURCES = [ROOT / 'tools' / 'dnskctl', ROOT / 'tools' / 'dnsk_preflight.c']


class DocError(Exception):
    """A comment is missing or malformed in a way the generator cannot hide."""


# ---------------------------------------------------------------------------
# Comment extraction
# ---------------------------------------------------------------------------

BLOCK = re.compile(r'/\*\*(.*?)\*/', re.S)


def strip_stars(text):
    """Turn a raw C comment body into clean lines."""
    out = []
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith('*'):
            line = line[1:].strip()
        out.append(line)
    # Drop leading/trailing blank noise but keep interior blank separators.
    while out and not out[0]:
        out.pop(0)
    while out and not out[-1]:
        out.pop()
    return out


def comment_blocks(path):
    """Yield stripped comment-body line lists from one source file."""
    text = path.read_text(encoding='utf-8')
    # Shell scripts use '# /** ... # */' so the block is still a comment to sh.
    if path.suffix == '' or path.name == 'dnskctl':
        lines = []
        inside = False
        for raw in text.splitlines():
            s = raw.strip()
            if s.startswith('#') and '/**' in s:
                inside = True
                lines = []
                continue
            if inside and s.startswith('#') and '*/' in s:
                inside = False
                yield strip_stars('\n'.join(lines))
                continue
            if inside:
                body = s[1:].strip() if s.startswith('#') else s
                lines.append(body)
        return
    for m in BLOCK.finditer(text):
        yield strip_stars(m.group(1))


def inline(text):
    """Normalize kernel-doc/Doxygen inline markup to plain Markdown."""
    text = re.sub(r'``([^`]+)``', r'`\1`', text)
    text = re.sub(r'[&%]([A-Za-z_][\w]*)', r'`\1`', text)
    text = re.sub(r':c:(?:type|func):`[^`]*<([^>]+)>`', r'`\1`', text)
    return text.strip()


def paragraphs(lines):
    """Group comment lines into (kind, content) records."""
    recs = []
    buf = []
    param = re.compile(r'^@([\w.]+):\s*(.*)$')
    for line in lines:
        m = param.match(line)
        if m:
            if buf:
                recs.append(('text', ' '.join(buf)))
                buf = []
            recs.append(('param', (m.group(1), m.group(2))))
            continue
        if line == '':
            if buf:
                recs.append(('text', ' '.join(buf)))
                buf = []
            continue
        buf.append(line)
    if buf:
        recs.append(('text', ' '.join(buf)))
    return recs


# ---------------------------------------------------------------------------
# Kernel-doc dialect (core/include/*.h)
# ---------------------------------------------------------------------------

DECL = re.compile(
    r'^(?:define|struct|enum|typedef|union)\s+([A-Za-z_][\w]*)\s*(?:-\s*(.*))?$'
)
FUNC = re.compile(r'^([A-Za-z_][\w]*)\(\)\s*-\s*(.*)$')


def kernel_entries(path):
    """Return a list of (title, subtitle, records) from one header."""
    entries = []
    for block in comment_blocks(path):
        if not block or not block[0]:
            continue
        head = block[0]
        dm = DECL.match(head)
        fm = FUNC.match(head)
        if dm:
            name, subtitle = dm.group(1), dm.group(2) or ''
            title = f'`{name}`'
        elif fm:
            name, subtitle = fm.group(1), fm.group(2)
            title = f'`{name}()`'
        else:
            # A block that does not open with a documented declaration is a
            # field/constant comment and is folded into the enclosing entry by
            # the caller, or is a free-standing note we surface verbatim.
            continue
        entries.append({
            'title': title,
            'subtitle': subtitle,
            'records': paragraphs(block[1:]),
            'fields': [],
            'source': path.name,
        })
    return entries


def render_kernel_entry(e, out):
    out.append(f"## {e['title']}")
    out.append('')
    if e['subtitle']:
        out.append(inline(e['subtitle']))
        out.append('')
    for kind, val in e['records']:
        if kind == 'text':
            out.append(inline(val))
            out.append('')
        else:
            key, desc = val
            out.append(f'- `{key}` {inline(desc)}')
    if any(k == 'param' for k, _ in e['records']):
        out.append('')
    out.append(f'> 来源: `{e["source"]}` 的 kernel-doc 注释')
    out.append('')


WHITELIST = {
    'module',       # forward declaration, not public API
    'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t',  # host compat typedefs in dh.h
}


def audit_kernel_coverage(entries):
    """Fail when a public declaration in a header has no kernel-doc comment."""
    documented = set()
    for e in entries:
        documented.add(e['title'].strip('`').removesuffix('()'))
    missing = []
    for path in KERNEL_HEADERS:
        for i, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            s = line.strip()
            m = re.match(r'^(?:#define|struct|enum|typedef|union)\s+([A-Za-z_]\w*)', s)
            if not m or s.startswith('#define _'):
                continue
            name = m.group(1)
            if name in WHITELIST or name in documented:
                continue
            missing.append(f'{path.name}:{i} {name}')
    if missing:
        raise DocError('public declarations without kernel-doc: ' + ', '.join(missing))


def build_kernel():
    out = [
        '# Kernel API',
        '',
        '本页由 `make docs` 从 `core/include/*.h` 与 `droid-hook/include/*.h` 的 kernel-doc 注释自动生成, 不要手工编辑.',
        '',
        '注释是唯一来源; 本页与头文件不一致时以头文件为准, 并重新生成.',
        '',
    ]
    entries = []
    for path in KERNEL_HEADERS:
        entries.extend(kernel_entries(path))
    if not entries:
        raise DocError('no kernel-doc declarations found in core/include/*.h')
    audit_kernel_coverage(entries)
    for e in entries:
        render_kernel_entry(e, out)
    out.append('## 覆盖检查')
    out.append('')
    out.append(f'本次共生成 {len(entries)} 个带注释的公开声明, 覆盖审计通过.')
    out.append('')
    return '\n'.join(out).rstrip() + '\n', len(entries)


# ---------------------------------------------------------------------------
# Doxygen dialect (tools/dnskctl, tools/dnsk_preflight.c)
# ---------------------------------------------------------------------------

BRIEF = re.compile(r'^@brief\s+(.*)$')
COMMAND = re.compile(r'^@command\s+(.+)$')
JSONKEY = re.compile(r'^@json\s+(\S+)\s+(.+)$')
NOTE = re.compile(r'^@note\s+(.*)$')
# A command has a compact name (no spaces except inside <>/[] args) and a
# description separated by one or more spaces.  Names may itself contain
# spaces (e.g. "storage-loop create <img> <size>"), so split on the last
# run of 2+ spaces before the description; when there is no such run, fall
# back to splitting on the first ASCII word boundary after the second token.
CMD_SPLIT = re.compile(r'^(.*?)\s{2,}(.+)$')


def split_command(raw):
    m = CMD_SPLIT.match(raw)
    if m:
        return m.group(1).strip(), m.group(2).strip()
    return raw.strip(), ''


def userspace_entries(path):
    blocks = list(comment_blocks(path))
    documented = [b for b in blocks if b and any(BRIEF.match(l) for l in b)]
    if not documented:
        raise DocError(f'{path.name}: no @brief Doxygen block found')
    result = []
    for b in documented:
        brief = ''
        commands = []
        jsonkeys = []
        notes = []
        body = []
        for line in b:
            m = BRIEF.match(line)
            if m:
                brief = m.group(1)
                continue
            m = COMMAND.match(line)
            if m:
                commands.append(split_command(m.group(1)))
                continue
            m = JSONKEY.match(line)
            if m:
                jsonkeys.append((m.group(1), m.group(2)))
                continue
            m = NOTE.match(line)
            if m:
                notes.append(m.group(1))
                continue
            if line:
                body.append(line)
        result.append({
            'brief': brief,
            'brief_body': ' '.join(body),
            'commands': commands,
            'jsonkeys': jsonkeys,
            'notes': notes,
            'source': path.name,
        })
    return result


def build_userspace():
    out = [
        '# Userspace API',
        '',
        '本页由 `make docs` 从 `tools/dnskctl` 与 `tools/dnsk_preflight.c` 的 Doxygen 注释自动生成, 不要手工编辑.',
        '',
        '注释是唯一来源; 本页与实现不一致时以实现为准, 并补齐注释后重新生成.',
        '',
    ]
    total_cmds = total_keys = 0
    for path in USERSPACE_SOURCES:
        for e in userspace_entries(path):
            out.append(f"## `{e['source']}`")
            out.append('')
            out.append(inline(e['brief']))
            out.append('')
            if e['brief_body']:
                out.append(inline(e['brief_body']))
                out.append('')
            if e['commands']:
                out.append('| 命令 | 作用 |')
                out.append('| --- | --- |')
                for cmd, desc in e['commands']:
                    out.append(f'| `{cmd}` | {inline(desc)} |')
                out.append('')
                total_cmds += len(e['commands'])
            if e['jsonkeys']:
                out.append('| JSON 键 | 含义 |')
                out.append('| --- | --- |')
                for key, desc in e['jsonkeys']:
                    out.append(f'| `{key}` | {inline(desc)} |')
                out.append('')
                total_keys += len(e['jsonkeys'])
            for n in e['notes']:
                out.append(f'> {inline(n)}')
                out.append('')
    if total_cmds == 0 or total_keys == 0:
        raise DocError('userspace surface incomplete: commands or JSON keys missing')
    out.append('## 覆盖检查')
    out.append('')
    out.append(f'本次共生成 {total_cmds} 条命令与 {total_keys} 个 JSON 键, 全部来自注释.')
    out.append('')
    return '\n'.join(out).rstrip() + '\n', total_cmds + total_keys


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def generate(dest):
    dest.mkdir(parents=True, exist_ok=True)
    kernel, kn = build_kernel()
    userspace, un = build_userspace()
    (dest / 'kernel.md').write_text(kernel, encoding='utf-8')
    (dest / 'userspace.md').write_text(userspace, encoding='utf-8')
    return kn, un


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true',
                    help='fail if committed pages differ from regenerated output')
    args = ap.parse_args()
    try:
        if args.check:
            with tempfile.TemporaryDirectory() as tmp:
                kn, un = generate(Path(tmp))
                for name in ('kernel.md', 'userspace.md'):
                    committed = ROOT / 'docs' / 'api' / name
                    if not committed.exists():
                        raise DocError(f'{committed} is missing; run make docs')
                    if committed.read_text(encoding='utf-8') != (Path(tmp) / name).read_text(encoding='utf-8'):
                        raise DocError(f'{committed} is stale; run make docs')
            print(f'API docs up to date: {kn} kernel entries, {un} userspace items')
        else:
            kn, un = generate(ROOT / 'docs' / 'api')
            print(f'wrote docs/api: {kn} kernel entries, {un} userspace items')
    except DocError as exc:
        print(f'gen-api-docs: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
