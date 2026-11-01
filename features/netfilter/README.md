# dnsk_netfilter (M2 development)

Vendored 6.12 nftables and xtables units in one `dnsk_netfilter.ko`:
the nftables target subset, `xt_addrtype`, `xt_REJECT` and `xt_RECENT`.

## Scope

- Build target: android16-6.12 only. Not release-supported.
- Loader parameters: `gki=1 activate=1`; `activate=0` is a no-op.
- nftables: Docker 29.x target subset, not full nftables. The nft `reject`
  expression is separate and remains covered by the `nft_reject*` units;
  `xt_REJECT` only serves iptables/ip6tables.
- `xt_addrtype`: iptables/ip6tables address-type match.
- `xt_REJECT`: iptables and ip6tables REJECT target. Linux 6.12 has no
  `net/netfilter/xt_REJECT.c`; upstream `ipt_REJECT.c` (NFPROTO_IPV4) and
  `ip6t_REJECT.c` (NFPROTO_IPV6) are vendored and registered together as the
  single `xt_reject` unit.
- `xt_RECENT`: iptables/ip6tables "recently seen" match, revisions 0 and 1,
  including per-net `/proc/net/xt_recent` state.
- Registration order is in `vendor/netfilter/units.h`; initialization walks it
  forward and teardown walks it in reverse. The two new xt units are appended
  after the nft units because they depend only on x_tables core, and keeping
  them last leaves the existing nft registration order unchanged. There is no
  xt/nft ordering dependency.
- The new units are vendored but not yet covered by device smoke tests; extend
  `tests/device/netfilter-*.sh` before claiming them supported.
- Native `xt_REJECT`/`xt_recent` must not already be built in or loaded: a
  duplicate target/match name makes registration fail with `-EEXIST`, and an
  existing `/proc/net/xt_recent` directory makes the recent per-net init fail.
  This is upstream all-or-nothing behavior, not masked here.
- Vendor provenance, upstream commit and pristine hashes: `ORIGIN.md`. These
  units only use symbols exported by 6.12; the existing KernelSU loader
  resolution rules for private nftables imports are unchanged.

## Status and device notes

- Inactive (`activate=0`) unload previously NULL-dereferenced in
  `nft_unregister_expr`; guarded by a `registered` flag. Regression test is
  `tests/device/netfilter-inactive-smoke.sh`.
- 设备 6.12.30 MTK 的 `struct xt_match/xt_target` 是 104 字节
  (`CONFIG_NETFILTER_XTABLES_COMPAT=n`); 设备包必须用
  `DNSK_XT_NOCOMPAT=1` 构建. `core/include/dnsk_xt_layout.h` 用静态断言锁定
  全部字段偏移, 防止再次出现 128 字节数组被 104 字节步长遍历的 panic.
  `xt_recent` registers an `xt_match` array and `xt_reject` registers two
  `xt_target`s, so both are affected by this layout rule.
- `xt_recent` also dereferences `net->proc_net` directly for
  `/proc/net/xt_recent`. The current preflight covers only `xt_match`/
  `xt_target`, so a device/VM smoke test that creates and removes a recent
  table is still needed.
