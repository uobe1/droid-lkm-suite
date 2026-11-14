# ABI 与 profile

读者: 修改 core API, 改动依赖内核结构的 feature, 或为新设备/KMI 构建的人
读完应能判断何时升 ABI, 何时重新生成 profile, 以及不匹配时系统怎样拒绝

## `DNSK_CORE_ABI` 与两层 ABI

`DNSK_CORE_ABI` 是 `struct dnsk_core_api` 的修订号 (当前为 5), 不是内核 ABI,
而是 **core 与 feature 之间的契约版本**.`dnsk_core_get(abi, size)` 要求精确
相等, 不做兼容回退; API 的布局或语义一变, 这个值就必须递增

一次成功的取表要过三重校验: `magic == DNSK_CORE_MAGIC`(`0x444E534B`);
`abi` 与调用方编译时的值完全一致; `size` 等于
`sizeof(struct dnsk_core_api)`.`dnsk_core_get()` 内部已做这三项; 若通过其他
途径拿到结构, 必须自己校验 `magic` 和 `abi` 后才能解引用函数指针
失败返回
`ERR_PTR`: core 未就绪是 `-ENODEV`, `abi`/`size` 不匹配是 `-EPROTO`.两种情况
都应让 feature 干净地加载失败, 而不是继续使用半个 API

core↔feature 由 `DNSK_CORE_ABI`+`magic`+`size` 保证, `abi`/`size` 不匹配时
`dnsk_core_get()` 返回 `-EPROTO`; 模块↔内核由 KMI/vermagic, BTF preflight,
build profile 保证, 不匹配时拒绝加载, 绕过检查则崩溃或写错字段

KMI 只冻结导出符号, 以及从这些符号可达的类型和签名; **未导出符号, 内部
struct 和 OEM 自行构建的内核不在冻结范围内**.因此 `uname`/KMI 相同不代表
struct 布局相同: 设备 6.12.30 的 `CONFIG_NETFILTER_XTABLES_COMPAT=n` 让
`xt_match/xt_target` 为 104 字节, 而 DDK/GKI 默认 128 字节; KernelSU 只解析
符号, 不检查布局.profile 存在的理由就是补上这一层

## profile 是什么

- `profiles/<kmi>.json`: 由 `tools/gen_profiles.py` 生成, 记录该 KMI 的原生
 能力矩阵和发布状态.`validated` 在证据附到 release 之前保持 false, 它本身
 不是"已验证"的声明
- `profiles/abi/<kmi>-<variant>.json`: **ABI 构建 profile**, 描述"哪套内核
 config 产生了这批 struct 布局".下文说的 profile 均指这一种
- ABI profile 的规则: schema 2 的 `config` 必须包含 registry 里的**全部** knob;
 缺失或未知 key 直接拒绝, 不回退默认值
- 只有可宏化的 knob 才输出构建变量: `DNSK_ABI_PROFILE`,
 `DNSK_XT_NOCOMPAT`/`DNSK_XT_LAYOUT`, `DNSK_PAGE_SIZE`, `DNSK_VA_BITS`,
 `DNSK_MAX_SKB_FRAGS`, `DNSK_CGROUP_SUBSYS_COUNT`; 其余 knob 由匹配 config
 的 DDK/内核构建保证, 不能用 shim 伪装
- 6.12 有 `android16-6.12-xt104.json`(设备默认, `xtables_compat=0`,
 `DNSK_XT_LAYOUT=104`) 和 `android16-6.12-xt128.json`(stock GKI/DDK,
 `xtables_compat=1`, `DNSK_XT_LAYOUT=128`, `validated: false`).
- profile 构建时选择; CI 只对 `android16-6.12` 矩阵应用, 未建模 KMI 不得复用;
 设备包必须选与设备 BTF 一致的 profile, 不能只看 KMI 名

## 哪些 config 会改变结构布局

下列类别与构建内核不一致, 就可能让模块写到错误偏移 ("处理方式"来自 ABI
影响面清单):

| 类别 | 代表 config | 影响的边界结构 | 处理方式 |
|---|---|---|---|
| xtables | `NETFILTER_XTABLES_COMPAT` | `xt_match`/`xt_target` 104↔128 | 唯一允许用头文件 shim 归一化的项 |
| 锁/调试 | `PREEMPT_RT`, `LOCKDEP`/`DEBUG_LOCK_ALLOC`, `DEBUG_MUTEXES`, `DEBUG_SPINLOCK`, `DEBUG_RWSEMS`, `MUTEX_SPIN_ON_OWNER` | `mutex`/`rw_semaphore`/`spinlock`/`wait_queue_head`/`task_struct` | 必须匹配内核 config, 不能 shim |
| 内存检查/随机化 | `KASAN` 模式, `KCSAN`, `KMSAN`, `KFENCE`, `RANDSTRUCT` | `task_struct` 调试字段/重排, `inode`/`pid_namespace` | 匹配模式; RANDSTRUCT 无精确 seed 则 unsupported |
| OEM 保留 | `ANDROID_VENDOR_OEM_DATA`(不是 `ANDROID_OEM_DATA_ARRAY`) | mutex/task/sk_buff/skb_shared_info/rw_semaphore | 必须匹配 |
| 网络 | `NET_NS`, `NF_CONNTRACK`(+MARK/SECMARK/ZONES/LABELS), `NF_TABLES`, `IP_VS`, `XFRM`, `WEXT_CORE` | `net`, `nf_conn` | 必须匹配 |
| skb | `SKB_EXTENSIONS`, `MAX_SKB_FRAGS` | `sk_buff`, `skb_shared_info` | 必须匹配 |
| 文件系统/安全 | `TMPFS_QUOTA`, `POSIX_ACL`, `SECURITY`/SELINUX, `SYSCTL`, `MEMFD_CREATE`, `KEYS`, `BSD_PROCESS_ACCT` | `inode`, `shmem_inode_info`, ns/cred/key 结构 | 必须匹配 |
| cgroup/调度 | `CGROUPS` 及 controller 集, `CGROUP_BPF`, `PSI`, `CGROUP_FREEZER`, `MEMCG` | `cgroup`, `cgroup_subsys`, task 字段, `rhashtable.alloc_tag` | 必须匹配 |
| 架构/工具链 | `COMPAT`, `PAGE_SIZE`, `ARM64_VA_BITS`, `MODVERSIONS`, `CFI_CLANG`, `DEBUG_INFO_BTF` | `task_struct` 等; CRC/间接调用; preflight 依赖 BTF | 独立构建 + 匹配 DDK 工具链; BTF 缺失即 fail-closed |

## profile 修不了的情况

`cgroup_subsys` 设备 256 字节, DDK 264 字节 (DDK +48 有 `css_killed`,
`can_fork`/`fork`/`exit` 偏移各差 8).这是**源码/backport 差异, 不是 config
差异**, profile 无法修复; 用 DDK 头构建 cgpids 会把回调写错位置
必须用设备
侧 KDIR/头文件 overlay 或源码 shim, 并由 preflight `--feature cgpids` 校验设备
BTF(256, `can_fork@104`, 无 `css_killed`).

## ABI 戳如何校验, 以及不匹配的表现

- 构建: profile 把可宏化值写入 `.ko` stamp(`dnsk_abi_profile=`,
 `dnsk_xt_layout=`); `core/include/dnsk_xt_layout.h` 对 104/128 静态断言
 struct 大小和全部字段偏移, 断言不过即构建失败
- 打包: `package-dev` 用同一 profile 把 `abi_profile`/`xt_layout` 写入
 `policy.conf`; `tools/package.py` 比对模块 stamp 并写入 manifest, 不一致不出包
- 加载: `dnskctl load ipset|netfilter` 先比对 `.ko` stamp 与 `policy.conf`,
 不一致或缺字段即拒绝; 通过后 `dnsk-preflight` 从设备 BTF 校验 size 与全部
 成员偏移, 失败不调用 `ksud insmod`.两道检查都过才允许加载
 不匹配时:

- **core ABI**: `dnsk_core_get()` 返回 `-EPROTO`, feature 应直接失败, 不再触碰 API
- **profile/stamp**: `dnskctl` 在 insmod 前拒绝, 这是 fail-closed 的预期;
 不要提供强制加载绕过
- **绕过检查直接 insmod**: 128 布局的 xt 模块在 104 设备上会在
 `xt_register_matches` 崩溃 (已发生过).
- **源码差异当 config**: cgpids 会把回调写错偏移, 可能到 fork/exit 才暴露,
 比直接崩溃更难查

## 什么情况下要重新生成或重新选择 profile

- 换 KMI/内核版本: 重新生成, 不能复用旧 profile
- 改任一 config knob, 或设备 BTF 的 size/偏移与当前 profile 不一致: 新增/
 修正 profile, 用实测值, 不猜测
- `xtables_compat`, `PAGE_SIZE`, `ARM64_VA_BITS`, `MAX_SKB_FRAGS`, cgroup
 controller 集变化: 对应 `DNSK_*` 与 stamp 会变, 必须重建
- 新增 feature 引入新的边界 struct: 同时加 preflight 检查和 profile knob;
 只加 feature 不加检查等于绕过 ABI 门
- 改 `struct dnsk_core_api` 的布局/语义: 递增 `DNSK_CORE_ABI`, 重编全部
 feature; 只改内部实现, 不动 API 和边界 struct 则不用升 ABI, 但仍要跑
 preflight
- profile 与 preflight 互补: profile 决定按哪套 config 构建, preflight 决定
 设备是否真的匹配

## 自查清单

- [ ] 改 `dnsk_core_api` 字段/语义或 `size`?→ 递增 `DNSK_CORE_ABI` 并同步调用方
- [ ] 新增/修改边界 struct → 更新 profile, 静态断言和 preflight
- [ ] 设备与 DDK 差异是 config 还是源码?源码差异不能靠 profile 解决
- [ ] `.ko` stamp, `policy.conf`, manifest 三处一致; 不匹配即拒绝加载

## 相关阅读

- [core API 契约](../../core/include/dnsk_core.h): ABI, magic, size 与错误语义
- `docs/evidence/build-profiles.md`(本地资料, 不随仓库分发): schema, CI 输入与 stamp 校验
- `docs/evidence/abi-config-scope.md`(本地资料, 不随仓库分发): 完整 config 清单与设备/DDK 差异
- [状态矩阵](../status.md): 当前 core ABI 与各 feature 的验证状态
