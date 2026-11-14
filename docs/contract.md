# 实现契约

本文件规定**代码必须满足什么**.不是使用说明, 是验收判据: 不满足即视为未完成

读者: 要改这个项目的人
使用者文档见 [docs/index.md](index.md).

---

## 一, 核心约束

### 1.1 Native 或 Real, 无 STUB / FAKE_SUCCESS

功能要么真实实现, 要么明确不实现

- 未实现的功能返回正确 errno 或不注册, **不得**返回成功
- **不得**用显示层, 空实现, "看起来能用" 掩盖缺失
- **不得**对外宣称未验证的能力

判据: 拿一个原生内核做同样的操作, 两者结果 (含 errno) 必须可比较

### 1.2 不改动不可逆部位

**不得**写 `boot` / `vendor_boot` / `dtbo` / `system` / `vendor_dlkm` / `system_dlkm`
**不得**把 SELinux 改为 permissive

### 1.3 加载方式

模块只能经 `ksud insmod` 或 `droid-lkm-ins`(`DNSK_LOADER=lkmins`) 加载
**不得**使用 `/system/bin/insmod` 或 busybox insmod 作为受支持路径

---

## 二, O / V 分类 (实现类型)

分类依据**两个层面的语义**, 不是"谁的代码在跑"

| 类别 | 定义 | 判据 |
|---|---|---|
| **O** | 内核态与用户态语义**都与原生相同** | 换成"打开该 `CONFIG_*` 重编的内核", 从 `/proc`, `/sys`, 结构布局到 errno, 计数, 都看不出差别 |
| **V** | **仅用户态语义相同**(或用户态也只部分) | 用户态看着一样, 但内核里没有真实子系统: `/proc/cgroups` 无该行, controller ID 不同, 计数由模块自己维护 |

**约束**:

- O 类**不得**只做到用户态一致就宣称完成 -- 那是 V 的定义
- V 类**不得**声称自己是 O
- 任何"以牺牲某项已有功能换取目标功能"的实现, **必须在文档中明确标出牺牲了什么**,
 并在状态表中记为已知缺口

---

## 三, 内存与生命周期

### 3.1 模块内存不能留给内核长期持有

模块 `.text` / `.data` / `.bss` 在 `rmmod` 后消失
因此:

- **不得**把长期存在的内核对象 (`file_system_type`, `pernet_operations`,
 `cgroup_subsys`, RCU 回调等) 放在模块内存里
- 需要长期存活的, 改为堆分配 (内存在, 指针在), 或把回调换成内核常驻函数
- RCU 延迟回调必须用 `kfree_rcu()` 这类内核侧释放, 不能用 `call_rcu()` +
 模块内的释放函数

判据: 假设某个延迟回调在 `module_exit` **之后**才执行, 它跳转到的地址还有效吗?

### 3.2 core 不 pin feature, feature 必须自己清理

- `feature_add()` / `hook_add()` **不**增加 owner 引用
- 所以 `rmmod` 会**成功**, 但 core 里会留下指向已释放代码的注册项
- 后果是**之后的 UAF / panic**, 不是卸载失败
- 因此 `module_exit()` 必须: 先 `hook_del()` 每个 hook → 再 `feature_del()` →
 最后 `module_put(api->owner)`

### 3.3 core 引用一对一

`dnsk_core_get()` 成功会取一份 core 引用

- 每条退出路径**恰好** `module_put(api->owner)` 一次 (含提前返回的失败分支)
- 最后一次 put 之后**不得**再触碰 API(含 `log`)
- 漏 put → core refcount 永不归零 → `rmmod dnsk_core` 永远 EAGAIN
- 提前 put 后继续用 → core 可被卸走 → UAF

### 3.4 自持引用必须能被释放

若模块为保活某资源而 `__module_get(THIS_MODULE)`:

- 必须提供一条**能返回错误**的路径来释放它 (模块参数, ioctl 等),
 因为 `module_exit` 不能失败
- 释放前必须先把资源归还 (槽位, css, mask, static key), 顺序不能颠倒
- 仍有活跃使用时该路径返回 `-EBUSY` 并保持模块加载

---

## 四, ABI 与 profile

### 4.1 两层 ABI 分开

| 层 | 由谁约束 | 变更后果 |
|---|---|---|
| `DNSK_CORE_ABI`(core ↔ feature) | 本项目 | 改 core API 布局或语义必须升 ABI, 所有 feature 同批重编 |
| 内核 ABI(内核 ↔ 模块) | KMI + 设备 config | 改边界 struct 或 config 必须重生成 / 重选 profile |

`dnsk_core_get()` 校验 `magic` + `abi` + `size` 三者, 任一不符返回 `-EPROTO`

### 4.2 影响结构布局的 config 必须进 profile

KMI **只冻结导出符号及其可达类型**, 不冻结内部 struct 布局
因此这些 config 必须作为 profile 参数而非常量:

- 锁 / 调试 / RT: `PREEMPT_RT`, `DEBUG_MUTEXES`, `LOCKDEP`, `KASAN` 等
- 厂商字段: `ANDROID_VENDOR_OEM_DATA`
- 网络: `NF_CONNTRACK` 系列, `MAX_SKB_FRAGS`
- 文件系统: `TMPFS_QUOTA`, `POSIX_ACL`
- cgroup: controller 数量, `CGROUP_BPF`, `PSI`
- `NETFILTER_XTABLES_COMPAT`(唯一可用头文件归一化的项)

### 4.3 fail-closed

profile 不匹配时**必须拒绝加载**, 不得"尽力而为":
未知 feature 不得跳过 stamp 校验, 缺失 key 不得回退到猜测值

---

## 五, 卸载安全

**`rmmod` 返回 0 不等于卸载安全.** 它只说明 refcount 归零

一个卸载算合格, 必须同时满足:

1. 无 holders(`/sys/module/<name>/holders/` 为空)
2. 确实不在 `lsmod`
3. 卸载后 dmesg 未出现新的 `BUG` / `WARNING` / `refcount_t` / `Oops` / `Call trace`
4. 卸载后内核行为回到原生态 (该子系统 / 文件系统 / 文件路径不再存在或行为如初)

不满足第 4 条但 1–3 条通过, 属于**静默损坏**, 必须修复而非接受

---

## 六, 证据要求

### 6.1 每条状态声明都要有证据

状态表里每一项必须有可执行的验收证据: QEMU 门控, 真机门控, 或差分结果
**没有证据时状态保持 `not-shipped`.**

### 6.2 证据与文档分层

| 位置 | 内容 | 跟踪 |
|---|---|---|
| tracked 文档 | 读者能直接用的内容 | 是 |
| `docs/notes/` | 一次性发现, 踩坑, provenance | 否 |
| `docs/evidence/` | 研究汇报, 门控证据, 可行性研究 | 否 |
| memory | 跨会话可复用的规则与事实 | 否 |

tracked 文档**不得**链接后三者 (新克隆会 404).只能写纯路径并注明"本地资料"

### 6.3 数字与 run id 不进 tracked 文档

它们属于 `docs/notes/` 或 `docs/evidence/`.tracked 文档只写结论与判据

---

## 七, 文档契约

### 7.1 三类文档各司其职

| 文档 | 给谁 | 内容 |
|---|---|---|
| `README.md` | 使用者 | 项目是什么, 怎么用 |
| `docs/index.md` `docs/guides/` | 使用者 | 操作步骤 |
| `docs/architecture/` | 实现者 | 机制与约束 |
| `docs/api/` | 实现者 | 接口契约 |
| `CONTRIBUTING.md` | 维护者 | 流程, 风格, 验收 |
| 本文件 | 实现者 + 维护者 | 必须满足的判据 |

**使用者文档里不写维护者契约**(分支策略, tag 规则, 风格要求, 提交规范).
读者不需要知道这些才能用项目

### 7.2 写之前核实

写"漏掉 X 会导致 Y"之前, **读代码确认 Y 真的是那样**
文档描述错误比没有文档更糟: 读者按它排查会走反方向

### 7.3 文档与代码同步

改了代码就检查哪份文档在描述它.CI 会校验生成页面与头文件一致
(`make docs-kernel` + `git diff --exit-code`).

---

## 八, 验收矩阵

一个 feature 算完成, 需同时满足:

| 项 | 判据 |
|---|---|
| 构建 | 8 个 KMI 各自构建通过 (或明确标注该 KMI 不支持) |
| ABI | preflight 通过, stamp 与 policy 一致 |
| 功能 | QEMU 门控 + 真机门控通过 |
| 语义 | O 类需内核态 + 用户态双向差分; V 类需明确标注差异点 |
| 卸载 | 第五节 4 条全部满足 |
| 文档 | 该 feature 的 README(含 Scope / Status / Non-goals / Evidence) |
| 状态 | `docs/status.md` 有对应行, 且状态与证据一致, 实现类别标 O 或 V |