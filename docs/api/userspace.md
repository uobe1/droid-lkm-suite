# Userspace API

本页由 `make docs` 从 `tools/dnskctl` 与 `tools/dnsk_preflight.c` 的 Doxygen 注释自动生成, 不要手工编辑.

注释是唯一来源; 本页与实现不一致时以实现为准, 并补齐注释后重新生成.

## `dnskctl`

dnskctl - device-side control CLI for droid-lkm-suite

The only supported user-space entry point for loading, unloading and inspecting modules.  It never writes boot partitions and never fakes status; every reported value comes from a real system read.

| 命令 | 作用 |
| --- | --- |
| `probe [--json]` | show device/KMI/ksud/cgroup facts |
| `cap [--json]` | show core capability/status |
| `status [--json]` | show loaded modules and /proc/dnsk/status |
| `monitor [--json]` | one machine-readable state snapshot per call |
| `start-all [--json]` | load every shipped module in dependency order |
| `stop-all [--json]` | unload features then core, verifying each step |
| `preflight [feature]` | run device-side preflight (default: core) |
| `load <feature>` | load a feature module via DNSK_LOADER (idempotent) |
| `unload <feature>` | unload via the same loader: refs, dependants, quiesce, post-check |
| `profile <name>` | load a named profile |
| `enable|disable` | allow/skip core auto-load on the next boot |
| `logs [lines]` | tail the module state logs |
| `boot-begin|boot-ok|boot-check` | manage the boot state marker |
| `doctor [--last-panic]` | collect dmesg/pstore/panic evidence |
| `cgroup-prep [--plan]` | detect cgroup v1/v2 paths; does not modify them |
| `cgroup-prep --bind <root>` | bind existing /dev cgroup dirs into <root> |
| `cgroup-prep --delegate <dir>` | enable controllers in subtree_control |
| `storage-loop create <img> <size> [ext4|f2fs]` | create a sparse loop image |
| `storage-loop remove <img>` | umount (best effort) and delete the image |

> --json prints a single-line JSON object; exit status still reflects errors.

## `dnsk_preflight.c`

dnsk-preflight - device-side fact check before a module is loaded

This is intentionally conservative: it checks the facts dnsk_core depends on and refuses to report success when a required symbol or environment fact is missing.  It emits a single-line JSON object on stdout; dnskctl load refuses to continue when @json ok is false. selected

| JSON 键 | 含义 |
| --- | --- |
| `ok` | overall result, false when any required check fails |
| `feature` | feature name being checked (default: core) |
| `release` | uname(2) release string |
| `kmi` | derived KMI generation string |
| `supported` | whether this KMI is in the supported list |
| `page_size` | kernel page size in bytes |
| `btf` | whether /sys/kernel/btf/vmlinux is usable |
| `ksud` | resolved ksud path; empty when absent or when the lkmins backend is |
| `btf_layout` | object: BTF structure sizes read from the device |
| `btf_layout.xt_match_size` | sizeof(struct xt_match) |
| `btf_layout.xt_target_size` | sizeof(struct xt_target) |
| `btf_layout.seq_file_private` | offset of seq_file->private |
| `btf_layout.xt_ok` | combined xtables layout verdict |
| `btf_layout.boundary` | object: layout boundary sub-checks |
| `btf_layout.boundary.locks` | lock field offsets verified |
| `btf_layout.boundary.checked_xt` | xt boundary check was requested |
| `btf_layout.boundary.xt_params` | xt per-parameter offsets verified |
| `btf_layout.boundary.skb` | sk_buff offsets verified |
| `btf_layout.boundary.net` | net offsets verified |
| `btf_layout.boundary.checked_fs` | fs boundary check was requested |
| `btf_layout.boundary.fs` | fs offsets verified |
| `btf_layout.boundary.checked_ns` | ns boundary check was requested |
| `btf_layout.boundary.ns` | ns offsets verified |
| `btf_layout.boundary.checked_cgpids` | cgpids check was requested |
| `btf_layout.boundary.cgpids` | cgpids offsets verified |
| `btf_layout.boundary.checked_tmpfsx` | tmpfsx check was requested |
| `btf_layout.boundary.tmpfsx` | tmpfsx offsets verified |
| `btf_layout.boundary.ok` | combined boundary verdict |
| `nf` | object: netfilter ABI sub-checks |
| `nf.checked` | netfilter check was requested |
| `nf.hook_ops` | nf_hook_ops layout verified |
| `nf.hook_state` | nf_hook_state layout verified |
| `nf.conn` | nf_conn layout verified |
| `nf.pernet_ops` | pernet_operations layout verified |
| `nf.nfnetlink` | nfnetlink_subsystem layout verified |
| `nf.nfnl_callback` | nfnl_callback layout verified |
| `nf.nat_hook` | nat hook layout verified |
| `nf.nat_range2` | nf_nat_range2 layout verified |
| `nf.rhashtable` | rhashtable layout verified |
| `nf.ok` | combined netfilter verdict |
| `symbols` | object: required symbol name -> present boolean |
| `checks` | object: boolean environment facts |
| `checks.page_size_4k` | page size is 4096 |
| `checks.btf` | BTF available |
| `checks.kmi_supported` | KMI supported |
| `checks.ksud` | ksud present in the selected backend (false under lkmins) |
| `checks.dcache` | dcache available |
| `checks.kallsyms_resolver` | kallsyms resolver available |
| `checks.xt_layout` | xtables layout acceptable |
| `checks.boundary` | boundary checks acceptable |
| `checks.nf_abi` | netfilter ABI acceptable |
| `checks.nf_symbols` | netfilter symbols present |
| `checks.cgpids_symbols` | cgpids symbols present |
| `checks.tmpfsx_symbols` | tmpfsx symbols present |
| `checks.seq_private` | seq_file private offset acceptable |

## 覆盖检查

本次共生成 19 条命令与 55 个 JSON 键, 全部来自注释.
