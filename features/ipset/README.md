# dnsk_ipset (development)

Linux 6.12 上游 `CONFIG_IP_SET` + `IP_SET_HASH_IP` + `IP_SET_HASH_NET` +
`NETFILTER_XT_SET` 的真实实现，链接为单个 `dnsk_ipset.ko`。目标是 arm64
Android GKI KMI `android16-6.12`。源码来自 AOSP 固定 tag，见
[ORIGIN.md](ORIGIN.md)。没有 STUB/FAKE 路径；未实现的部分直接报错。

当前状态：开发实现，未验收，不得据此声称已支持。

## 构建

```sh
# stock-GKI/QEMU: kernel has CONFIG_NETFILTER_XTABLES_COMPAT=y (128-byte xt_match)
make -C features/ipset KDIR=/opt/ddk/kdir/android16-6.12 modules

# target 6.12.30 MTK device: XTABLES_COMPAT=n (104-byte xt_match/xt_target)
make -C features/ipset KDIR=/opt/ddk/kdir/android16-6.12 DNSK_XT_NOCOMPAT=1 modules
```

设备构建通过 `compat_include` shim 在首次包含 `x_tables.h` 前关闭
`CONFIG_NETFILTER_XTABLES_COMPAT`; `main.c` 的静态断言固定 104 字节及全部
字段偏移, 防止再次出现内核按 104 步长遍历 128 数组导致的 panic。
输出 `features/ipset/dnsk_ipset.ko`。CI 的 `android16-6.12` 构建与 QEMU 产物
会编译本模块；DDK 构建结果与导出检查以 CI 日志为准。

## 支持的语义

### nfnetlink 核心

- 注册 `NFNL_SUBSYS_IPSET`（协议 7，最低兼容协议 6）、per-net 状态和
  `SO_IP_SET` getsockopt。
- 支持 `ipset` 工具使用的核心命令：`protocol`、`create`、`destroy`、
  `flush`、`rename`、`swap`、`list`、`save`、`add`、`del`、`test`、
  `header`、`type`、`get_byname`、`get_byindex`。
- 每个 netns 独立；默认最多 256 个 set（`CONFIG_IP_SET_MAX=256`），加载时
  可用模块参数 `max_sets` 覆盖（运行时 0600，可写）。
- 元素级语义、错误码、引用计数和 GC 均走 6.12 上游实现，不是裁剪版。

### set 类型

| 类型 | revision | 说明 |
|---|---|---|
| `hash:ip` | 0-6 | IPv4/IPv6 地址；IPv4 支持 CIDR/范围，rev 6 支持 `bitmask` |
| `hash:net` | 0-7 | IPv4/IPv6 前缀；rev 1 起 IPv4 支持范围输入，rev 2 起支持 `nomatch` |

`create` 支持的属性（按类型/上游 revision 生效）：

- `hashsize`、`maxelem`、`initval`、`bucketsize`、`timeout`；
- `counters`、`comment`、`skbinfo`、`forceadd`；
- `hash:ip`：`netmask`（IPv4/IPv6）或 `bitmask`（IPv4/IPv6，二者互斥）。

`add`/`del`/`test` 支持：

- `hash:ip`：单地址；IPv4 还支持 `IP_TO`/CIDR 范围（单条命令受
  `IPSET_MAX_RANGE` 限制）；IPv6 仅单地址；
- `hash:net`：地址/CIDR；IPv4 支持 `IP_TO` 范围展开；`nomatch` 元素；
- 扩展属性：`timeout`、`bytes`/`packets`（需 `counters`）、`comment`
  （需 `comment`）、`skbmark`/`skbprio`/`skbqueue`（需 `skbinfo`）、
  `lineno`；
- `list`/`save` 输出与上游一致，包含 set 头、元素、扩展和 timeout 剩余值。

`resize` 属性在 6.12 上游仍被 create 策略接受，但内核已改为自动扩容，
不再有独立的 resize 开关行为。

### 默认值与边界

- `hashsize` 默认 1024，小于 64 时按 64 处理；`maxelem` 默认 65536。
- `bucketsize` 有效范围 2..12，奇数向上取偶；`initval` 未指定时随机生成。
- `timeout` 以秒计，超过 `IPSET_MAX_TIMEOUT` 时由内核截断。
- 单条 IPv4 范围展开超过 `IPSET_MAX_RANGE`（16384）时返回 `-ERANGE`。

### xt_set

- `set` match：IPv4 rev 0-4，IPv6 rev 1-4；支持 `src`/`dst` 维度选择、
  `--return-nomatch`、`--update-counters`、`--update-subcounters`；
  `hash:ip`/`hash:net` 都是 1 维 set，`src,dst` 等多维元素不属于这两个
  类型；
- `SET` target：IPv4 rev 0-3，IPv6 rev 1-3；支持 `--add-set`、`--del-set`、
  `--map-set`；
- 仅 `NFPROTO_IPV4`/`NFPROTO_IPV6`，不注册 ARP/bridge family。

## 不支持

- 其他 set 类型：`bitmap:ip`、`bitmap:ip,mac`、`bitmap:port`、`hash:ip,mac`、
  `hash:ip,mark`、`hash:ip,port`、`hash:ip,port,ip`、`hash:ip,port,net`、
  `hash:mac`、`hash:net,iface`、`hash:net,net`、`hash:net,port`、
  `hash:net,port,net`、`list:set`。`create` 这些类型返回
  `IPSET_ERR_FIND_TYPE`，不会创建空壳。
- nftables 原生 `set` 表达式：本模块只做 xtables/ipset，不是 nftables。
- `ipset` 用户态对内核模块的自动加载：必须先手动加载 `dnsk_ipset.ko`；
  之后 `request_module("ip_set_hash:ip")` 一类别名可能命中，但设备上不能
  依赖 depmod。
- 与厂商内核自带 `CONFIG_IP_SET=y/m` 混用。此时
  `nfnetlink_subsys_register()` 返回 `-EBUSY`，模块加载回滚，不会覆盖原生
  实现。`NETFILTER_XT_SET` 在 Kconfig 中依赖 `IP_SET`，因此不存在“只有
  内置 xt_set、没有 ipset 核心”的正常配置。
- 未验证的组合：加载其他独立 ipset 类型模块到本核心之上；ARP/bridge 上的
  set 匹配；32 位内核；非 6.12 KMI。

## 卸载与安全

- 每个 set 通过 `set->type->me` 持有本模块引用；每条使用 `set` match 或
  `SET` target 的 iptables/ip6tables 规则也持有引用。
- 因此有 set 或规则时 `rmmod dnsk_ipset` 返回 `EBUSY`；先 `ipset destroy`
  全部 set 并删除相关规则，才能卸载。卸载顺序为 xt_set -> set 类型 ->
  核心，逐层反向回滚，与上游多模块的依赖顺序一致。
- 加载失败时按相反顺序回滚已注册的单元，不留下半注册状态。

## 测试计划

以下全部为待执行项，不能当作已通过。测试环境为 arm64 目标机或 QEMU
GKI；用户态需要 `ipset` 7.x（protocol 6/7）以及带 `xt_set` 扩展的
legacy `iptables`/`ip6tables`，Android 自带 rootfs 通常不含这些工具。

1. 构建：在 `ddk-min:android16-6.12` 中执行构建命令；`modinfo` 检查
   `license: GPL`、`parm: max_sets`、`alias: nfnetlink-subsys-6` 等；确认
   `modpost` 未报未导出符号。
2. 加载/卸载：无 set 时加载成功；`rmmod` 成功；创建 set 后 `rmmod`
   返回 `EBUSY`；`ipset destroy` 后成功卸载；重复加载/卸载 10 次无泄漏。
3. `hash:ip` IPv4/IPv6：create/add/test/del/flush/list/save/restore；
   重复 add 返回 `-EEXIST`，删除不存在元素返回 `-IPSET_ERR_EXIST` 语义；
   IPv4 范围与 CIDR；IPv6 单地址。
4. `hash:net`：`10.0.0.0/8` 匹配 `10.1.2.3`；`nomatch` 子网在 `ipset
   test` 和 `-m set --return-nomatch` 下的差异；IPv4 范围输入；IPv6 前缀。
5. 扩展：`timeout` 到期后 test 失败且 GC 回收；`counters` 在真实流量下
   增长；`comment`/`skbinfo` list/save 往返；`bitmask`/`netmask` 互斥。
6. `set` match：IPv4/IPv6 的 src/dst、`--return-nomatch`、计数器更新；
   用实际报文验证 DROP/ACCEPT 结果。
7. `SET` target：`--add-set`/`--del-set`/`--map-set` 与 `hash:ip`、
   `hash:net` 组合；规则删除后引用计数归零。
8. netns：两个 netns 中同名 set 互不影响；删除 netns 后其 set 被销毁，
   模块引用计数下降。
9. 容量与并发：`maxelem` 上限、`hashsize`/`bucketsize` 边界、大量元素
   dump/restore、GC 与 test 并发压力。
10. 差分：同 KMI 的 Native oracle（内核自带 `CONFIG_IP_SET` + `ipset`
    工具）与 LKM 环境使用同一 rootfs 和同一命令序列，比较退出码、
    `ipset list` 字段与 `iptables -L -v` 计数器。
11. 卸载安全：set、iptables 规则、netns 三种引用分别存在时验证
    `EBUSY`，全部释放后卸载成功。

## 构建风险

- 本仓库没有本地内核树，未编译；必须先由 DDK 构建证明。
- 依赖 GKI 导出的 `nfnetlink_subsys_register/unregister`、
  `nfnetlink_unicast`、`nfnl_lock/unlock`、`nf_register_sockopt`/
  `nf_unregister_sockopt`、`xt_register_matches`/`xt_unregister_matches`、
  `xt_register_targets`/`xt_unregister_targets` 等。`gki_defconfig` 中
  `NF_CT_NETLINK=y` 会 select `NETFILTER_NETLINK`，`NETFILTER_XT_MATCH_*`
  会 select `NETFILTER_XTABLES`，因此这些符号应在 `vmlinux` 中；但 DDK
  `Module.symvers` 是否包含它们需以首次 CI 构建为准。
- 源码 pin 为 6.12.38，设备报告 6.12.30；stable 6.12 内 KMI 不变，
  `ip_set_core.c` 与 `v6.12.38` 逐字节一致，仍需按实际 DDK 版本构建。
- 需使用 DDK 的 Clang/CFI 配置构建；普通主机 `gcc` 产物不能用于 GKI。
- 上游内核并不导出全部被引用符号；目标设备由 KernelSU 的 `ksud insmod`
  解析，普通 `/system/insmod` 不能加载。
- 若厂商内核已内置 ipset/xt_set，加载会失败；先用 `/proc/config.gz` 与
  `lsmod` 确认。
