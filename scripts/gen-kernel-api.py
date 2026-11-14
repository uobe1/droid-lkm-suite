#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn kernel-doc rST output into a readable HTML page.

kernel-doc emits reStructuredText.  Rendering that properly needs Sphinx, which
this project does not run; publishing raw rST to a Pages site shows the reader
directives instead of prose.  This script converts the small subset kernel-doc
actually produces -- section titles, parameter lines and body paragraphs -- into
plain HTML, so the committed page under `docs/api/kernel/` is a real artifact
rather than a hand-written stub.

It is deliberately not a general rST parser.  If kernel-doc starts emitting
constructs it does not understand, they end up as plain paragraphs; that is a
visible defect, not silently wrong markup.

Usage:
    gen-kernel-api.py <input.rst> <output.html>
"""

import html
import re
import sys

HEAD = """<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8">
<title>Kernel API</title>
<style>
body{font-family:system-ui,-apple-system,sans-serif;max-width:52rem;margin:2rem auto;
     padding:0 1rem;line-height:1.65;color:#1a1a1a}
code{background:#f4f4f5;padding:.1rem .3rem;border-radius:3px;font-size:.92em}
h3{margin-top:2.2rem;border-top:1px solid #e5e5e5;padding-top:1.1rem}
p.param{margin:.15rem 0;color:#444}
p.param code{background:none;color:#0a5;font-weight:600}
</style></head><body>
<h1>Kernel API</h1>
<p>由 <code>core/include/dnsk_core.h</code>、<code>dnsk_abi.h</code> 及各 layout
头文件的 kernel-doc 注释生成。权威定义始终是头文件本身；本页与头文件不一致时
以头文件为准。</p>
"""

# kernel-doc emits ".. c:function:: NAME", ".. c:type:: struct foo" and friends.
DIRECTIVE = re.compile(r'^\.\. c:(?:function|type|struct|enum|macro|var)::\s*(.+)$')
# A DOC: section renders as an anchor line plus a bold title, not as a c: directive.
DOC_ANCHOR = re.compile(r'^\.\. _(.+?):?\s*$')
DOC_TITLE = re.compile(r'^\*\*(.+?)\*\*\s*$')
# Section markers kernel-doc inserts that mean nothing to a reader.
NOISE = re.compile(
    r'^(\.\. container::|\.\. c:struct::|\.\. c:enum::|\.\. c:macro::|'
    r'\.\. c:var::|^\s+:\w+:|^\*\*(Description|Parameters|Return)\*\*$)')
PARAM = re.compile(r'^\s+:([\w\[\]]+):\s*(.*)$')


def clean_name(raw: str) -> str:
    """Reduce kernel-doc's rendered signature to the symbol a reader searches for."""
    name = raw.strip()
    name = re.sub(r'\s*\(.*\)\s*$', '', name)          # drop trailing (args)
    if name.startswith('struct '):
        # "struct dnsk_core_api * dnsk_core_get" -> "dnsk_core_get"
        parts = name.split()
        if '*' in name or len(parts) > 2:
            name = parts[-1]
        else:
            name = parts[-1]
    elif name.startswith('enum '):
        name = name.split()[-1]
    return name.strip()


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]

    out = [HEAD]
    body: list[str] = []
    params: list[str] = []
    symbols = 0

    def flush() -> None:
        nonlocal body, params
        for p in params:
            out.append(p)
        if body:
            text = ' '.join(body)
            out.append(f'<p>{html.escape(text)}</p>')
        body, params = [], []

    with open(src, encoding='utf-8') as fh:
        pending_doc = None
        for line in fh:
                line = line.rstrip('\n')
                dm = DOC_ANCHOR.match(line)
                if dm:
                    pending_doc = dm.group(1)
                    continue
                tm = DOC_TITLE.match(line)
                if tm and pending_doc:
                    flush()
                    symbols += 1
                    raw_title = tm.group(1)
                    title = html.escape(raw_title)
                    # kernel-doc uses the whole "name - description" line as the anchor;
                    # an id containing spaces is not a usable anchor, so keep the name.
                    anchor = raw_title.split(' - ')[0].strip()
                    out.append(f'<h3 id="{html.escape(anchor)}">{title}</h3>')
                    pending_doc = None
                    continue
                m = DIRECTIVE.match(line)
                if m:
                    flush()
                    symbols += 1
                    sym = clean_name(m.group(1))
                    out.append(f'<h3 id="{html.escape(sym)}"><code>{html.escape(sym)}</code></h3>')
                    continue
                if NOISE.match(line) or '**Parameters**' in line or '**Return**' in line:
                    continue
                # An rST section underline is decoration, not content.
                if re.fullmatch(r'[-=~^]{3,}', line.strip()):
                    continue
                pm = PARAM.match(line)
                if pm:
                    name, desc = pm.group(1), pm.group(2).strip()
                    if name in ('param', 'returns', 'return'):
                        body.append(desc)
                    else:
                        params.append(
                            f'<p class="param"><code>{html.escape(name)}</code> '
                            f'{html.escape(desc)}</p>')
                    continue
                if not line.strip():
                    flush()
                    continue
                body.append(line.strip())
    flush()

    out.append('</body></html>')
    with open(dst, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(out) + '\n')
    print(f'{dst}: {symbols} symbols')
    return 0


if __name__ == '__main__':
    sys.exit(main())