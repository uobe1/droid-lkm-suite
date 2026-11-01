# dnsk_netfilter (M2 development)

Vendored Droidspaces 6.12 nftables/xt_addrtype units.

- Build target: android16-6.12 only.
- Loader parameters: `gki=1 activate=1`; `activate=0` is a no-op.
- Not yet loaded or claimed supported.
- Inactive (`activate=0`) unload previously NULL-dereferenced in
  `nft_unregister_expr`; guarded by a `registered` flag. Regression test is
  `tests/device/netfilter-inactive-smoke.sh`.
- Scope: Docker 29.x target subset, not full nftables.

- 设备 6.12.30 MTK 的 `struct xt_match/xt_target` 是 104 字节
  (`CONFIG_NETFILTER_XTABLES_COMPAT=n`); 设备包必须用
  `DNSK_XT_NOCOMPAT=1` 构建. `core/include/dnsk_xt_layout.h` 用静态断言锁定
  全部字段偏移, 防止再次出现 128 字节数组被 104 字节步长遍历的 panic.
