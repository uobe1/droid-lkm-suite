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

## xt_REJECT / xt_RECENT 真机结果 (2026-11-01)

使用 CI run `41268020282` 的 2636a95 构建 (仅 lint 失败, build/package/guards
通过), 设备 boot_id `49afc24f-...`:

```text
dnsk-preflight --feature netfilter ... -> ok:true,
  boundary/nf/xt_layout/nf_symbols 全 true
dnskctl preflight netfilter -> rc=0 (stamp 校验通过)
Loaded kernel module: .../dnsk_netfilter.ko
addrtype_registered=ok
xt_recent_registered=ok
xt_recent_proc=ok
xt_reject_available=ok
xt_addrtype_rule=ok
FAIL: netfilter module survived unload
```

**卸载回归与根因:** 设备原生已注册 IPv4/IPv6 REJECT。旧构建仍注册自己的
REJECT target; `xt_register_target()` 把新 target 插到链表头, 下一次
filter 表替换时, 设备已有的 `-j REJECT` 规则被重新解析到我们的 target,
每条规则持有一个模块引用, rmmod 返回 EBUSY (refcnt 3, 后续 6). 这与
`xt_register_target()` 自身不取引用一致.

**修复:** `ipt_REJECT.c`/`ip6t_REJECT.c` 先用 `xt_request_find_target()`
探测, 存在原生 target 时释放探测引用并跳过注册; `-EEXIST` 作为兜底同样
跳过. `xt_recent` 仍按 EEXIST 处理. 这样设备上 REJECT 保持 Native, 模块只
提供缺失的 recent, 卸载路径不再被规则 pin.

**复测 (CI run 41269098493, commit 2738366, boot_id
`aa1f26d0-ea6d-4caf-8d95-6d25a953eb1f`):**

```text
insmod dnsk_netfilter.ko gki=1 activate=1
dnsk_netfilter 413696 0
refcnt=0
dnsk_netfilter: xt_REJECT IPv4 already provided by the kernel; skipping
dnsk_netfilter: xt_REJECT IPv6 already provided by the kernel; skipping
rmmod -> rc=0

active smoke:
addrtype_registered=ok
xt_recent_registered=ok
xt_recent_proc=ok
xt_reject_available=ok
xt_reject_native=ok
xt_addrtype_rule=ok
netfilter_unload=ok
NETFILTER_ACTIVE_SMOKE_OK
```

复测后 boot_id 未变, 无 dnsk 残留, `/proc/net/xt_recent` 随卸载移除.
设备上的 dmesg `Call trace` 来自厂商 `iorap2` 模块, 与本套件无关.

## ABI 边界复核

设备 BTF 与 DDK 6.12.76 模块 DWARF 对比 (2026-11-01, 104 构建):

| struct | 设备 BTF | 结论 |
|---|---|---|
| `nf_hook_ops` | 40, hook 0, dev 8, priv 16, pf 24, hook_ops_type 25:8, hooknum 28, priority 32 | 一致 |
| `nf_conn` | 272, status 128, nat_bysource 144, master 160, mark 168, secmark 172, ext 176, proto 184 | 一致 (MARK/SECMARK=y, ZONES=n) |
| `pernet_operations` | 72, init 16, pre_exit 24, exit 32, exit_batch 40, exit_batch_rtnl 48, id 56, size 64 | 一致 |
| `nfnetlink_subsystem` | 64, owner 24, commit 32, abort 40, valid_genid 48 | 一致 |
| `nfnl_callback` | 32, call 0, policy 8, type 16, attr_count 20 | 一致 |
| `net` | 4160, gen 2848 | 一致 |
| `rhashtable` | 160 (alloc_tag 152) | 一致 |
| `xt_match`/`xt_target` | 104 | 见 xt-layout-preflight |
| `nf_hook_state` | 48, hook 0, pf 1, in 8, out 16, sk 24, net 32, okfn 40 | 一致 |
| `nf_nat_hook` | 32, 3 个函数指针 0/8/16 | 一致 |
| `nf_nat_range2` | 44, flags 0, min/max_addr 4/20, proto 36/38/40 | 一致 |
| `sk_buff` | 248, sk 24, _nfct 104, tail/end/head/data 208-224 | 一致 |
| `skb_shared_info` | 360, frags 88 | 一致 |
| 锁原语 | mutex 48, rw_semaphore 64, spinlock 4, wait_queue_head 24, timer_list 40, work_struct 32 | 一致 (LOCKDEP/DEBUG_* 关闭) |

`dnsk-preflight --feature netfilter` 现在同时校验上述 size/偏移, 以及
`nf_nat_masquerade_inet_register_notifiers`, `nf_nat_register_fn`,
`xt_register_matches`, `xt_unregister_matches`, `nfnetlink_subsys_register`,
`rhashtable_init_noprof` 六个符号. `--feature ipset` 校验 xt 回调参数
(`xt_action_param`/`xt_mtchk_param`/`xt_tgchk_param`)、skb 与 net; 所有
feature 都校验锁原语布局. 设备 BTF 实测 `boundary.*`, `nf.*` 与
`nf_symbols` 全为 `true`; 合成 BTF 中把 `nf_nat_range2` 的 `base_proto` 或
`net.gen` 偏移改错会分别使 `nf_abi`/`boundary` 变为 `false`.

若设备已有原生 nftables 注册, `nfnetlink_subsys_register` 返回 `-EBUSY`,
模块回滚并让 insmod 失败, 不会伪装成功.

## 边界

- 设备没有 nft/ipset 用户态工具; nftables 规则、Docker/UFW/Fail2ban
  工作负载尚未验收.
- 这次只验证 active 注册、addrtype match 与卸载; 不代表完整 M2 门控通过.
