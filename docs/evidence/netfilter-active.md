# dnsk_netfilter 真机 active 验证

设备: 6.12.30-android16-5 MTK, `CONFIG_NETFILTER_XTABLES_COMPAT=n`
(`struct xt_match/xt_target` 104 字节). 使用 CI run 472f 的 104 字节
no-compat 构建, 设备 boot_id `49afc24f-33b7-4121-9665-1195ff11210d`.

## preflight

静态 arm64 `dnsk-preflight` 读取设备 BTF:

```json
{"ok":true,"release":"6.12.30-android16-5-g1ed949324a3e-ab13881345-4k",
 "kmi":"android16-6.12","btf_layout":{"xt_match_size":104,
 "xt_target_size":104,"xt_ok":true},"checks":{"xt_layout":true,...}}
```

## active load/unload

`dnsk_netfilter.ko gki=1 activate=1`, 无规则:

```text
Loaded kernel module: .../dnsk_netfilter.ko
netfilter_registration=ok
xt_addrtype_registered=ok
NETFILTER_LOAD_SMOKE_OK
```

模块随后卸载, boot_id 未变, 无 Oops.

## xt_addrtype 规则

使用设备自带 `/system/bin/iptables` 创建临时链并添加
`-m addrtype --src-type LOCAL`:

```text
addrtype_registered=ok
xt_addrtype_rule=ok
nft_unavailable=skip
netfilter_unload=ok
NETFILTER_ACTIVE_SMOKE_OK
```

无 dnsk 模块残留, boot_id 未变.

## 边界

- 设备没有 nft/ipset 用户态工具; nftables 规则、Docker/UFW/Fail2ban
  工作负载尚未验收.
- 这次只验证 active 注册、addrtype match 与卸载; 不代表完整 M2 门控通过.
