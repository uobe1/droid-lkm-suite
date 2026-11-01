# 设备 xt_set 注册 panic 取证 (2026-11-01)

## 现象

设备在一次启动约 48 分钟后 panic, boot reason 为 `kernel_panic`; 重启后
root 仍可用, 当前 boot_id 已变化.

pstore 与 Android dropbox `SYSTEM_LAST_KMSG` 都能看到:

```text
Unable to handle kernel NULL pointer dereference at virtual address 000000000008
pc : xt_register_matches+0x50/0xb8
lr : xt_register_matches+0x44/0xb8
```

MTK AEE 目录 `/data/vendor/aee_exp/db.fatal.01.KE` 记录了同一事件
(`KE at xt_register_matches+0x50/0xb8`), 但 `.dbg` 是厂商加密格式,
设备上的 `aeev_v2`/`mrdump_tool` 不能解码; pstore 在寄存器 dump 处截断,
缺少 `Modules linked in` 与 `Comm:`.

## 关键判别

崩溃寄存器 `x19` 指向 `xt_match` 数组, `x20=7`; 在
`xt_register_matches(match, n)` 中第二个参数是 match 数量, 因此这次调用
注册的是 **7 个** match.

- Linux 6.12 上游 `xt_set` 与当前 `dnsk_ipset` 的 `set_matches[]` 是
  **9 个** revision (IPv4 rev0-4, IPv6 rev1-4), `set_targets[]` 是 7 个.
- 旧版 xt_set (如 5.x) 的 match 数组是 **7 个** (IPv4 rev0-3,
  IPv6 rev1-3), 其 `struct xt_match` 布局与 6.12 不同, 在 6.12 上注册会
  读错字段, 与 NULL@0x8 的现象一致.
- `xt_register_matches(match, n)` 的 `n` 在崩溃寄存器中为 7; 当前 6.12
  实现传入 9, 与现场不符.

因此这次 panic 的提供者更像一个**旧的/外来的 xt_set LKM**, 不是当前仓库
构建的 6.12 `dnsk_ipset`.

## 当前仓库侧核对

- `features/ipset/vendor/linux/net/netfilter/xt_set.c` 的 9 个 match 条目
  均包含 `name/family/revision/match/matchsize/me`, 7 个 target 条目均包含
  `name/family/revision/target/targetsize/me`, 没有空字段. 主机测试
  `tests/host/test_tools.py` 已固化该完整性检查.
- 设备 `/data/adb/modules` 没有 `droid_lkm_suite`; `/data/local/tmp` 与
  Termux home 没有 `dnsk_ipset.ko`; 标准与 vendor 模块目录没有
  `xt_set.ko`/`ip_set*.ko`.
- 构建该 ipset 的 agent 工具记录中没有 ssh/scp/insmod/ksud 操作.
- 同一份 `dnsk_ipset.ko` 在 stock GKI 6.12.76 QEMU 中成功注册
  `xt_set` 并完成 hash:ip/hash:net 语义测试, 没有 panic.

## 结论与边界

能证明的: 当前 `dnsk_ipset` 的注册数组没有空指针, 且 6.12 版本在受控
GKI 上可正常注册; 设备 panic 的调用参数与当前实现不符.

不能证明的: 是哪一个外部/旧 LKM 触发了设备 panic; AEE 完整日志加密,
pstore 截断. 需要 MTK PC 端 AEE 解码工具或复现加载者才能定论.

设备侧的旧 `xt_set.ko` 已被移除或从未保存在常见路径; 在找到来源前, 不应
在真机加载任何来源不明的 xt_set/ipset 模块.
