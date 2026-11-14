#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn kernel-doc rST into the Markdown page MkDocs renders.

kernel-doc emits reStructuredText. This project documents everything in Markdown
through MkDocs, so the generated API page must be Markdown too: an HTML page
would bypass the theme, the navigation and the search index.

The converter handles the small subset kernel-doc produces: ".. c:function::"
and ".. c:type::" entries, ".. _anchor:" plus bold title for DOC sections,
":param:" lines and body paragraphs. Anything else becomes a plain paragraph,
which is a visible defect rather than silently wrong markup.

Usage:
    gen-kernel-api.py <input.rst> <output.md>
"""

import re
import sys

ENTRY = re.compile(r'^\.\. c:(?:function|type|struct|enum|macro|var)::\s*(.+)$')
DOC_ANCHOR = re.compile(r'^\.\. _(.+?):?\s*$')
DOC_TITLE = re.compile(r'^\*\*(.+?)\*\*\s*$')
PARAM = re.compile(r'^\s+:([\w\[\]]+):\s*(.*)$')
NOISE = re.compile(r'^(\.\. container::|\.\. c:struct::|\.\. c:enum::|\.\. c:macro::|\.\. c:var::)')
UNDERLINE = re.compile(r'^[-=~^]{3,}$')

HEADER = """# Kernel API

本页由 `make docs-kernel` 从 `core/include/*.h` 的 kernel-doc 注释生成, 不要手工编辑.

权威定义是头文件本身; 本页与头文件不一致时以头文件为准, 并重新生成.

"""


def symbol_of(raw):
    """Reduce kernel-doc's rendered signature to the name a reader searches for."""
    name = raw.strip()
    name = re.sub(r'\s*\(.*\)\s*$', '', name)
    name = name.split('*')[-1].strip() if '*' in name else name
    for prefix in ('struct ', 'enum ', 'union '):
        if name.startswith(prefix):
            return name.split()[-1]
    return name


def main():
    if len(sys.argv) != 3:
        print('usage: gen-kernel-api.py <input.rst> <output.md>', file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]

    out = [HEADER]
    body, params = [], []
    count = 0

    def flush():
        nonlocal body, params
        out.extend(params)
        if body:
            out.append(' '.join(body))
            out.append('')
        body, params = [], []

    with open(src, encoding='utf-8') as fh:
        pending = None
        for raw in fh:
            line = raw.rstrip('\n')
            dm = DOC_ANCHOR.match(line)
            if dm:
                pending = dm.group(1)
                continue
            tm = DOC_TITLE.match(line)
            if tm and pending:
                flush()
                count += 1
                out.append(f'## {tm.group(1)}')
                out.append('')
                pending = None
                continue
            em = ENTRY.match(line)
            if em:
                flush()
                count += 1
                out.append(f'## `{symbol_of(em.group(1))}`')
                out.append('')
                continue
            if NOISE.match(line) or UNDERLINE.match(line.strip()):
                continue
            if line.strip() in ('**Parameters**', '**Description**', '**Return**'):
                continue
            pm = PARAM.match(line)
            if pm:
                key, desc = pm.group(1), pm.group(2).strip()
                if key in ('param', 'return', 'returns'):
                    body.append(desc)
                else:
                    params.append(f'- `{key}` {desc}')
                continue
            if not line.strip():
                flush()
                continue
            body.append(line.strip())
    flush()

    with open(dst, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(out).rstrip() + '\n')
    print(f'{dst}: {count} entries')
    return 0


if __name__ == '__main__':
    sys.exit(main())
