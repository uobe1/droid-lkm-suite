# GKI 未冻结的 ABI 影响面

## KMI 冻结范围

AOSP 对每个 KMI 分支只保留一份 `gki_defconfig` 和一套 Clang 工具链。ABI
监控的集合是 `gki/aarch64/symbols/*` 中符号的并集，**加上从这些符号可达的
类型和签名**。未导出符号、内部 struct 和 OEM 自行构建的内核不在冻结范围内。
冻结后允许 vendor defconfig 片段存在，但不能改变 KMI 可达 ABI。

这解释了设备事实：MTK 6.12.30 没有导出 `xt_register_matches`，其
`struct xt_match` 为 104 字节，而 DDK/GKI 为 128 字节。KernelSU 只解析符号，
不检查 struct 布局。

## 已确认会影响边界布局的 config

| 类别 | 代表 config | 影响的边界结构 | 处理方式 |
|---|---|---|---|
| xtables | `NETFILTER_XTABLES_COMPAT` | `xt_match`/`xt_target` 104↔128 | 唯一可用头文件 shim 归一化的项 |
| 锁/调试 | `PREEMPT_RT`, `LOCKDEP`/`DEBUG_LOCK_ALLOC`, `DEBUG_MUTEXES`, `DEBUG_SPINLOCK`, `DEBUG_RWSEMS`, `MUTEX_SPIN_ON_OWNER` | `mutex`/`rw_semaphore`/`spinlock`/`wait_queue_head`/`task_struct` | 必须匹配内核 config，不能 shim |
| 内存检查 | `KASAN` 模式, `KCSAN`, `KMSAN`, `KFENCE` | `task_struct` 调试字段；KASAN 调用为代码级 | 匹配模式；BTF 不能区分 HW_TAGS 与关闭 |
| 布局随机化 | `RANDSTRUCT` | `task_struct`/`inode`/`pid_namespace` 等重排 | 无精确 seed 则 unsupported |
| OEM 保留 | `ANDROID_VENDOR_OEM_DATA`（不是 `ANDROID_OEM_DATA_ARRAY`） | mutex/task/sk_buff/skb_shared_info/rw_semaphore | 必须匹配 |
| 网络 | `NET_NS`, `NF_CONNTRACK`(+MARK/SECMARK/ZONES/LABELS), `NF_TABLES`, `IP_VS`, `XFRM`, `WEXT_CORE` | `net`, `nf_conn` | 必须匹配 |
| skb | `SKB_EXTENSIONS`, `MAX_SKB_FRAGS` | `sk_buff`, `skb_shared_info` | 必须匹配 |
| 文件系统/安全 | `TMPFS_QUOTA`, `POSIX_ACL`, `SECURITY`/SELINUX, `SYSCTL`, `MEMFD_CREATE`, `KEYS`, `BSD_PROCESS_ACCT` | `inode`, `shmem_inode_info`, ns/cred/key 结构 | 必须匹配 |
| cgroup/调度 | `CGROUPS` 及 controller 集, `CGROUP_BPF`, `PSI`, `CGROUP_FREEZER`, `CGROUP_SCHED`, `MEMCG`, `MEM_ALLOC_PROFILING` | `cgroup`, `cgroup_subsys`, task 字段, `rhashtable.alloc_tag` | 必须匹配 |
| 架构 | `COMPAT`, `PAGE_SIZE`, `ARM64_VA_BITS` | `task_struct` 等；分配/对齐宏 | 独立构建 + preflight |
| 工具链 | `MODVERSIONS`, `CFI_CLANG` | 无 struct 布局，影响 CRC/间接调用 | 必须匹配 DDK 工具链 |
| BTF | `DEBUG_INFO_BTF` | 无 ABI，preflight 依赖 | 缺失即 fail-closed |

`TRACEPOINTS`/`FTRACE`、`DEBUG_INFO`、`DEBUG_OBJECTS` 不改变本套件检查的
边界 struct；`DEBUG_OBJECTS_WORK` 会改 work-data 标志位，不能依赖这些位。

## 设备 vs DDK 6.12.76

比较 10,903 个同名 struct，整体 140 个不同；本套件边界集合中只有两个差异：

| 对象 | 设备 | DDK | 原因 |
|---|---:|---:|---|
| `xt_match`/`xt_target` | 104 | 128 | `NETFILTER_XTABLES_COMPAT` |
| `cgroup_subsys` | 256 | 264 | **源码/backport 差异，非 config** |

`cgroup_subsys`：DDK 在 +48 有 `css_killed`，设备没有；`can_fork` 104 vs
112、`fork` 120 vs 128、`exit` 128 vs 136。用 DDK 头构建 cgpids 会把回调写错
位置，profile 无法修复，必须使用设备侧 KDIR/头文件 overlay 或源码 shim。
`cgroup` 两者均为 1984 且偏移一致。

其余已核对边界：`mutex` 48、`rw_semaphore` 64、`spinlock` 4、
`wait_queue_head` 24、`task_struct` 5184、`net` 4160、`nf_conn` 272、
`nf_hook_ops` 40、`sk_buff` 248、`skb_shared_info` 360、`inode` 704、
`shmem_inode_info` 824、`pid_namespace` 136、`nsproxy` 72、`ipc_namespace`
1264，偏移均一致。

## Profile 策略

- schema 2 的 `config` 必须完整记录全部 knob；`tools/build_flags.py` 拒绝
  缺失/未知 key 和 KMI 不匹配。
- 只有 `xtables_compat` 允许头文件 shim；其余必须选择 config 匹配的
  DDK/内核构建。
- 可宏化的 knob 输出 `DNSK_*` 构建变量，并写入模块 stamp；
  `policy.conf` 记录 `abi_profile`/`xt_layout`，`dnskctl` 在 insmod 前比对
  stamp 并跑设备 BTF preflight。
- 未建模的 knob、缺失 BTF/struct/member、profile 不匹配一律 fail-closed。
- 当前 preflight 的锁/skb/net 期望值来自本设备实测（LOCKDEP/DEBUG_*/
  KASAN-GENERIC 关闭、OEM data 开启等）；其他布局的设备会被拒绝，必须先
  测量该设备 BTF 再新增 profile/检查，不能填入猜测值。
- `cgroup_subsys` 源码差异单列为 cgpids 阻塞项，不能用 profile 掩盖。

## 来源

- [Maintain a stable KMI](https://source.android.com/docs/core/architecture/kernel/stable-kmi)
- [Android kernel ABI monitoring](https://source.android.com/docs/core/architecture/kernel/abi-monitor)
- [GKI FAQ](https://source.android.com/docs/core/architecture/kernel/gki-faq)
- [android16-6.12 gki_defconfig](https://android.googlesource.com/kernel/common/+/refs/heads/android16-6.12/arch/arm64/configs/gki_defconfig)
