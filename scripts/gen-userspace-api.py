#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the userspace API page from the sources of truth.

The userspace surface is not a C library: it is the `dnskctl` command set and the
single-line JSON that `dnsk-preflight` emits. Documenting them by hand guarantees
drift, so this script reads them back from the code.

  dnskctl        command list comes from its `usage()` here-document
  dnsk-preflight JSON keys come from the printf format string in main()

Anything it cannot find is reported as missing rather than silently skipped.

Usage:
    gen-userspace-api.py <output.md>
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CTL = ROOT / 'tools' / 'dnskctl'
PREFLIGHT = ROOT / 'tools' / 'dnsk_preflight.c'

HEADER = """# Userspace API

本页由 `make docs-api` 从 `tools/dnskctl` 与 `tools/dnsk_preflight.c` 生成, 不要手工编辑.

命令的实现在脚本里; 本页与实现不一致时以实现为准, 并重新生成.

"""


def dnskctl_commands():
    """Pull the command table out of usage()'s here-document."""
    text = CTL.read_text(encoding='utf-8')
    m = re.search(r'usage\(\)\s*\{\s*cat <<\'EOF\'\n(.*?)\nEOF', text, re.S)
    if not m:
        return []
    rows = []
    for line in m.group(1).splitlines():
        line = line.rstrip()
        if not line.strip():
            continue
        if line.startswith('usage:') or line.rstrip().endswith('commands:'):
            continue
        stripped = line.strip()
        if stripped.startswith('--json'):
            continue
        rows.append(stripped)
    return rows


def preflight_keys():
    """Extract JSON keys from the preflight printf format string."""
    text = PREFLIGHT.read_text(encoding='utf-8')
    keys = []
    for m in re.finditer(r'\\"([a-z_0-9]+)\\":', text):
        k = m.group(1)
        if k not in keys:
            keys.append(k)
    return keys


def main():
    if len(sys.argv) != 2:
        print('usage: gen-userspace-api.py <output.md>', file=sys.stderr)
        return 2
    out = [HEADER]

    out.append('## dnskctl')
    out.append('')
    out.append('设备侧唯一受支持的用户态入口, 所有结果来自真实系统读取.')
    out.append('')
    cmds = dnskctl_commands()
    if not cmds:
        out.append('**未能从 `usage()` 提取命令表**, 需要检查脚本格式.')
    else:
        out.append('| 命令 | 作用 |')
        out.append('| --- | --- |')
        for c in cmds:
            m = re.match(r'^(\S+(?:\s+\S+)*?)\s{2,}(.+)$', c)
            if m:
                out.append(f'| `{m.group(1)}` | {m.group(2)} |')
            else:
                out.append(f'| `{c}` | |')
    out.append('')

    out.append('## dnsk-preflight')
    out.append('')
    out.append('加载前的事实校验, 输出单行 JSON; `ok` 为 false 时 `dnskctl load` 拒绝继续.')
    out.append('')
    keys = preflight_keys()
    if not keys:
        out.append('**未能从源码提取 JSON 键**, 需要检查格式字符串.')
    else:
        out.append('| 键 |')
        out.append('| --- |')
        for k in keys:
            out.append(f'| `{k}` |')
    out.append('')

    Path(sys.argv[1]).write_text('\n'.join(out).rstrip() + '\n', encoding='utf-8')
    print(f'{sys.argv[1]}: {len(cmds)} commands, {len(keys)} json keys')
    return 0


if __name__ == '__main__':
    sys.exit(main())
