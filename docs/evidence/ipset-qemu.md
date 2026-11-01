# dnsk_ipset QEMU 语义测试

## 构建

- KMI: android16-6.12, 构建于 DDK 容器; 产物 `dnsk_ipset.ko` 已加入
  `core-android16-6.12` artifact.
- `tools/exports_guard.py` + `tools/imports_guard.py` 通过: 这个模块的所有
  未定义符号都由 GKI 导出, 不依赖 KernelSU 的隐藏符号解析.
- 主机测试 `tests/host/test_tools.py` 固化 `set_matches[]` 9 条 /
  `set_targets[]` 7 条, 每条包含 `name/family/match|target/me`.

## 首次 QEMU 运行 (run 41254632488)

DDK Linux 6.12.76-4k + Alpine 3.20.3 initramfs:

```text
DNSK_QEMU_IPSET_BOOT 6.12.76-4k
dnsk_ipset: ipset protocol 7 with hash:ip, hash:net and xt_set registered
```

`ipset create/add/test/list/destroy` 的 IPv4 hash:ip 与 hash:net 用例全部
通过; 唯一的失败是销毁 set 后立即 `rmmod` 返回 EBUSY:

```text
rmmod: can't unload module 'dnsk_ipset': Resource temporarily unavailable
```

根因是 `ip_set_destroy` 对每个 set 使用 `call_rcu` 释放 `set->type->me`
引用; 测试在 RCU 回调前就执行了 `rmmod`. 测试已改为最多等待 10 秒重试,
并在加载后检查 `xt_set` match 是否出现.

## 重试结果 (run 41256636843)

等待 RCU 后重试卸载的版本已全部通过:

```text
DNSK_QEMU_XT_SET_OK
DNSK_QEMU_IPSET_OK
qemu_rc=0
```

最初该 run 的 boot job 被误判失败, 原因是 workflow 只匹配通用标记
`DNSK_QEMU_OK`, 而 active-ipset init 当时只打印 `DNSK_QEMU_IPSET_OK`.
统一标记并修复 QEMU 固件包后, 最终复跑 **run 41257449372 绿色通过**
(复用 run 41256636843 的 image artifact, 只重跑 boot job).

## 边界

已覆盖: 模块加载、`ipset` protocol 7 注册、`xt_set` match 出现、
hash:ip/hash:net 的 create/add/test(命中/未命中)/list/destroy、卸载.
未覆盖: 超时/计数器/comment/skbinfo、iptables `-m set` 规则、netns 隔离、
Docker/UFW/Fail2ban 工作负载. 这些仍待后续验收.
