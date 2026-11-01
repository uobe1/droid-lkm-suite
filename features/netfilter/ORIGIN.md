# dnsk_netfilter origin

Upstream: <https://github.com/Shikochin/Droidspaces-LKM>
Commit: `8c14b35ce353bf1ddab56150733854e3eb440d68` (v0.1.5)
License: GPL-2.0-only (see `vendor/COPYING`; vendored source keeps its SPDX).

Droidspaces-vendored paths: `vendor/netfilter/*` from `src/netfilter/*`. The
Linux 6.12 xt additions are documented in the section below.

Local changes:

- `Kbuild`/`Makefile` are project-specific; output is `dnsk_netfilter.ko`.
- `vendor/netfilter/main.c`: the PLZ110/VM exact-target gate is replaced with a
  `gki=1` + `6.12.*-android16-*` gate. `activate=0` loads as a no-op;
  `activate=1` registers the vendored nftables and xt units. A `registered` guard
  prevents the exit path from unregistering units that were never initialized;
  the missing guard caused a NULL dereference in `nft_unregister_expr` when an
  inactive instance was unloaded.
- Build target is android16-6.12 only for now. Not release-supported.

## xt_REJECT and xt_RECENT (added 2026-11-01)

Upstream: AOSP `kernel/common`, branch `android16-6.12`, commit
`105b5745f1d771060aa9fb7f75821f69bb2298fd`
(<https://android.googlesource.com/kernel/common/+/105b5745f1d771060aa9fb7f75821f69bb2298fd>).
Fetched through the GitHub mirror
(`aosp-mirror/kernel_common@105b5745f1d771060aa9fb7f75821f69bb2298fd`).
The same files at `torvalds/linux` tags `v6.12` and `v6.12.30` are
byte-identical.

Linux 6.12 does not ship `net/netfilter/xt_REJECT.c`: the xtables REJECT
target is split into `net/ipv4/netfilter/ipt_REJECT.c` (NFPROTO_IPV4) and
`net/ipv6/netfilter/ip6t_REJECT.c` (NFPROTO_IPV6). Both are vendored and
exposed as the single `xt_reject` unit by project glue in
`vendor/netfilter/xt_reject.c`, which registers IPv4 first, rolls it back if
IPv6 registration fails, and unregisters IPv6 before IPv4.

Vendored paths and pristine SHA-256 (before the local wrapper change):

- `net/netfilter/xt_recent.c` -> `vendor/netfilter/upstream/xt_recent.c`
  `896244906b6440d5d016c5344ffc64570101e1ad06464082bd6c99461d4484c4`
- `net/ipv4/netfilter/ipt_REJECT.c` -> `vendor/netfilter/upstream/ipt_REJECT.c`
  `6e3982d84d561b3c8a6da34d7814d007f330f698b5e236b31e094a901897d977`
- `net/ipv6/netfilter/ip6t_REJECT.c` -> `vendor/netfilter/upstream/ip6t_REJECT.c`
  `1b4c56b8e04a7372c9eec6c16ba04c8074dd323d580bd09f3daac3b0e2a202e5`

The vendored copies keep their upstream SPDX/license notices. Their only local
changes are at the end: `module_init`/`module_exit` are replaced by
`ds_nf_init_*`/`ds_nf_exit_*` wrappers so the combined module has one initcall,
and `__exit` is dropped from the static teardown helper because the non-`__exit`
wrapper calls it (the same adaptation used by the existing units).
`vendor/netfilter/xt_reject.c` is project glue and has no upstream counterpart.

`xt_recent.c`'s `recent_mt_check_v0()` refers to `struct xt_recent_mtinfo_v0`,
which 6.12 headers do not define. Upstream only uses it as an opaque pointer and
copies `sizeof(struct xt_recent_mtinfo)`, so the incomplete type is valid C; it
is intentionally left as upstream wrote it.

Required headers are not vendored because the pinned DDK kernel tree already
provides them, exactly as for the existing nft_reject units:
`linux/netfilter/xt_recent.h`, `linux/netfilter_ipv4/ip_tables.h`,
`linux/netfilter_ipv4/ipt_REJECT.h`, `linux/netfilter_ipv6/ip6_tables.h`,
`linux/netfilter_ipv6/ip6t_REJECT.h`, `net/netfilter/ipv4/nf_reject.h` and
`net/netfilter/ipv6/nf_reject.h`.

Symbols used by these units are exported by the 6.12 kernel
(`xt_register_target`, `xt_unregister_target`, `xt_register_matches`,
`xt_unregister_matches`, `xt_check_proc_name`, `nf_send_unreach`,
`nf_send_reset`, `nf_send_unreach6`, `nf_send_reset6`, the proc/seq helpers
and `register_pernet_subsys`). No stubs or fake fallbacks are added. If a
target GKI build omits any of them, the existing KernelSU loader symbol
resolution applies.
