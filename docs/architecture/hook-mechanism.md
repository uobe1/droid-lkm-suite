# Hook 机制

读者: 需要为 feature 选择 hook 类型, 并确保注销后可以安全卸载的开发者
读完
应能判断该用哪种 hook, 以及删除时为什么必须等 RCU

## 能力总览

core 的 hook API 只有四种类型 (`enum dnsk_hook_type`):

| 类型 | 替换/插入什么 | 请求字段 | 删除时恢复什么 | 适合 |
|---|---|---|---|---|
| `DNSK_HOOK_KPROBE` | 在 `symbol` 入口挂 kprobe pre-handler | `symbol`, `replacement` | `unregister_kprobe()`, 清理 `.addr` 与 `KPROBE_FLAG_DISABLED` | 低频路径; 只想在入口观察参数或改参数 |
| `DNSK_HOOK_SYSCALL` | 改写 `sys_call_table[syscall_nr]` 槽 | `syscall_nr`, `replacement` | 把原槽值写回 | syscall 热路径 |
| `DNSK_HOOK_DATA` | 改写一个指针槽 | `target`, `replacement` | 把旧值 (可以为 NULL) 写回 | 只替换 ops 表项/函数指针, 不需要改代码 |
| `DNSK_HOOK_KRETPROBE` | 在 `symbol` 上挂 entry + return handler | `symbol`, `target`(entry), `replacement`(return) | `unregister_kretprobe()` | 需要读取或改写函数返回值 |

这四种都通过 `dnsk_hook_add()` / `dnsk_hook_del()` 注册, 由 core 持有.feature
**不得**在 core 之外自行安装永久 hook; core 负责计数, 注销和恢复原值
若内核
已经提供了正式注册接口 (nft/xt/ipset, cgroup cftypes, pernet 等), 优先直接
注册: 这是最稳的路径, 失败时按逆序回滚, 不涉及代码改写

## 怎么选

1. **内核已有注册接口** → 直接注册, 不要为了统一而套 hook
2. **系统调用热路径**(fork/exit 这类高频入口)→ `SYSCALL`.槽替换一次完成,
 命中成本最低; 配合 `patch_memory` + `stop_machine` 改写, preflight 要记录并
 校验槽原值
3. **低频路径或中心函数, 只需入口介入** → `KPROBE`.开销高于槽替换, 但不需要
 改 syscall 表, 也不需要复制整个函数体
4. **需要函数返回值**(例如判断某次迁移是否成功)→ `KRETPROBE`.返回值在
 return handler 里用 `regs_return_value()` 从 `regs` 读取
5. **只是替换 ops 表里的一个函数指针**(例如 `inode_operations` 的 xattr/ACL
 槽)→ `DATA`.不要用 kprobe 去模拟一次指针替换

## 各类型的语义

### KPROBE

`replacement` 的类型是 `int (*)(struct kprobe *, struct pt_regs *)`.返回非 0
表示跳过被 probe 的指令, 返回 0 继续执行.`original_out` 收到的是被 probe 的
**地址**, 不是原函数指针--kprobe 没有"原函数"可调用
注册失败时请求无效返回
`-EINVAL`, 其余错误由 kprobe 注册结果决定

### SYSCALL

`replacement` 是新的 syscall 函数体, `original_out` 收到被替换的槽值
槽值不能
为 NULL; patching 后端不可用时返回 `-EOPNOTSUPP`.写入走 patch 后端
(`stop_machine` + fixmap), 因此即使 syscall 表位于 init 后只读内存也可以改
这是热路径的首选, 但也是 preflight 必须核对"槽原值"的原因: 槽已经被别人改过
时, 直接覆盖会丢失对方的 hook

### DATA

`target` 指向要覆盖的指针槽, `replacement` 是新值
旧值允许为 NULL(例如内核
编译时缺失的 inode operation 槽), `dnsk_hook_del()` 会把记录的 NULL 原样写
回
写入同样走 `patch_memory`: 页表遍历 + fixmap + `copy_to_kernel_nofault` +
`stop_machine`.适合只换指针, 不改指令的场景

### KRETPROBE

`target` 是 entry handler, `replacement` 是 return handler, 签名都是
`int (*)(struct kretprobe_instance *, struct pt_regs *)`.返回值的读取发生在
return handler 中, 用 `regs_return_value()` 从该 handler 的 `regs` 取.entry
handler 返回非 0 时该实例不会建立返回回调, 适合"只对特定调用跟踪返回值"

## 注册与删除的生命周期

- 在 `module_init()` 里, `dnsk_core_get()` 成功之后注册; 在
 `feature_add()` 之前完成, 保证 `dnskctl status` 看到的 feature 已经可用
- key 是删除时的唯一凭据, 必须稳定且短于 `DNSK_HOOK_KEY_LEN`(48, 含 NUL).
 重复 key 返回 `-EBUSY`, 表满返回 `-ENOSPC`
- `owner` 必填但只用于诊断; core 不在 owner 上取引用, 因此不会替 feature 保活
 或自动注销
 带着 active hook 卸载 feature, core 注册表仍指向已释放的模块
 text--下一次命中就是 use-after-free
- 删除顺序与注册相反.`hook_del()` 先恢复被替换的值, 成功后再等一个 RCU 宽限
 期才返回; 返回后 handler 数据才可以释放
 删除失败返回 `-EINVAL`(key 为
 NULL), `-ENOENT`(没有该 key), 或恢复原值时的错误
- 任何一步失败都要逆序回滚已经成功的注册; 失败路径不能留下半套 hook

## `synchronize_rcu()` 的作用

`stop_machine()` 只能保证"恢复入口的那一刻, 没有 CPU 正停在被 hook 的函数
里".它不能保证一个在恢复前刚进入的 CPU 已经离开 trampoline, 而 trampoline
位于模块内存
若此时直接返回并允许 rmmod, 那个 CPU 会继续跳进即将消失的
text

因此 `hook_del()` 在恢复原值之后, 返回之前调用 `synchronize_rcu()`.效果是:

- 调用方在 `hook_del()` 返回后可以立即释放 handler 数据, 不会再有 RCU 读侧
 临界区引用它;
- 卸载路径不会与"刚进入 trampoline 的调用者"竞争;
- 代价是 rmmod 路径最多可能阻塞几十毫秒
- 这是可接受的: rmmod 不是热路径,
 正确性优先
 如果使用场景要求亚毫秒级卸载, 必须改成用 RCU 回调延后释放
 trampoline, 而不是删掉这次等待

`synchronize_rcu()` 只覆盖"正在执行 hook"这一种延迟, 不覆盖 task_work, RCU
回调, superblock 销毁等**在模块退出之后才运行**的路径
后者见
[卸载安全](unload-safety.md).

## 自查清单

- [ ] 先问有没有正式注册接口; 有就直接注册, 不套 hook
- [ ] 热路径用 `SYSCALL`; 只需入口介入用 `KPROBE`; 需要返回值用 `KRETPROBE`;
 只换指针用 `DATA`
- [ ] 每个 hook 有唯一, 稳定, 短于 48 字节的 key
- [ ] 注册在 `core_get` 之后, `feature_add` 之前; 删除在 `module_put` 之前,
 且逆序
- [ ] `hook_del()` 返回前不释放 handler 数据; 返回后才释放
- [ ] 不依赖 core 自动清理 hook; core 不 pin owner
- [ ] 若 hook 会触发延迟执行的回调, 按卸载安全检查表处理, 而不是只加
 `synchronize_rcu()`

## 相关阅读

- [core API 契约](../../core/include/dnsk_core.h): `enum dnsk_hook_type` 与
 `struct dnsk_hook_req` 的字段语义
- [卸载安全](unload-safety.md): 延迟回调与 module text 的关系
- [模块生命周期](module-lifecycle.md): 注册顺序与引用计数责任
