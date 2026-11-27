# 卸载安全

读者: 新增 feature, 修改退出路径, 或准备宣称"这个模块可以 rmmod"的人
读完
应知道 rmmod 返回 0 只是起点, 以及模块要满足什么条件才算卸载安全

## "能 rmmod"不等于"卸载安全"

`rmmod` 返回 0 只证明模块 refcount 降到了 0, 内核执行了 `module_exit()`.它不
证明没有别的 task/回调/子系统持有指向模块内存的指针, 不证明入口, ops 表和注册
项都已恢复, 也不证明卸载后行为回到原生态

安全卸载的目标是: **每个模块都能离开, 内核继续运行, 任何其他 task 可达的
东西都不再指向随模块释放的内存**.验收至少三项: `rmmod` 成功, `lsmod`/sysfs
清洁, 原生态行为恢复 (PID/USER_NS 不再可创建, tmpfs 重新拒绝 `user.*`).

## 先审计: 谁把模块 pin 住了

自持引用是最常见的"能加载但永远不能卸载"来源
它不是安全措施, 而是死锁:
refcount 永远不归零, `rmmod` 永远 `EAGAIN`, 而唯一会释放引用的 `module_exit()`
永远不会运行

| 模块 | 原 pin | 处理方式 |
|---|---|---|
| `dnsk_ns` | hook 安装时 `__module_get()`, 外加每个活跃 pid/ipc namespace 一份引用 | 去掉安装期 pin; namespace 引用随 namespace 释放经 RCU 掉落 |
| `dnsk_devtmpfs` | init 时 `__module_get()`, 配一个空的 `module_exit()` | 去掉 pin, 把 exit 原本假装做的 teardown 真正实现 |
| `dnsk_tmpfsx` | init 时 `try_module_get()`(理由: superblock 持有指针) | 去掉; exit 本来就会从每个 tmpfs superblock 解掉 `s_xattr` |
| `dnsk_cgpids` native | init 时 `try_module_get()`, 真实需要 | 拆成 quiesce + exit 两步 |
| `dnsk_cgpids` virtual | per-cgroup state 的 `call_rcu()` 回调在模块里, 因此自持 | 改为 `kfree_rcu()`, 回调由内核常驻代码执行, 不再自持 |

新增模块时, 对每处 `module_get`/`try_module_get` 都要能回答: 为什么必须自持?
能否改成"对称注销 + 不 pin"?若必须, 在取引用处写清"不可直接 rmmod"及原因

## 延迟回调与 module text

模块 `.text` 在 `module_exit()` 返回后不再映射
任何**在那之后才运行**, 函数体
又在模块里的回调, 都会跳进已释放的内存
来源包括: `call_rcu()` 回调,
`task_work`(例如 `mntput()` 排队的 `cleanup_mnt`), workqueue/timer, 注销后仍
可能被读到的 netns/pernet 结构, superblock 的延迟销毁路径

处理方式只有三种: ① 在 `module_exit()` 里同步 drain; ② 把回调或函数指针换成
内核常驻代码; ③ 自持引用并承认不可卸载 (最后手段, 必须在取引用处记录).

`dnsk_cgpids` virtual 是第 ② 种: per-cgroup state 原由模块内的 `call_rcu()`
回调释放, 宽限期可能在 `module_exit()` 后结束; 改用 `kfree_rcu()` 后由内核
常驻代码释放.`rcu_head` 必须在 offset 0, 这是 `kfree_rcu()` 快路径的要求

`dnsk_devtmpfs` 是这条规则的反例:

- 模块从 `internal_fs_type`(位于模块 `.bss`) 挂载私有 superblock;
- `mntput()` 不同步释放 superblock, 而是把 `cleanup_mnt` 排到 task_work;
- 之后 `deactivate_locked_super()` 读取 `s_type` 并调用 `s_type->kill_sb()`;
 此时模块已卸载, `s_type` 指向已释放内存, 直接 panic

`stop_machine()` 和 `synchronize_rcu()` 都救不了: 故障在 task_work 回调里, 不是
正在执行的 hook.只替换 `kill_sb` 也不够--崩在 `deactivate_locked_super+0x74`
的是**读取 `s_type` 字段本身**; struct 没了, 后续 teardown 都会 fault

## 模块内存不能留给内核长期持有

结论: **模块内存是唯一会消失的东西, 任何比模块活得久的结构都不能放在那里**
堆内存能活, 内核 text 能活, `.bss`/`.data` 不能

devtmpfs 的修法不是"更小心地等", 而是改变所有权:

- `file_system_type` 用 `kzalloc()` 放堆; `owner` 保持 NULL, 模块不被 type 表 pin;
- `init_fs_context`/`kill_sb` 指向内核常驻函数;
- `module_exit()` **不释放**这两个 type struct, 延迟 teardown 因此没有可
 dangling 的对象; 代价是每次加载留下约 200 字节堆对象

`public_dev_mount()` 会把**同一个** superblock 返回给系统里每个
`mount -t devtmpfs`(`atomic_inc(&s->s_active)`).不能只拆自己的 mount 就离开:
先删 type 表项让新解析找不到它, 再在 text 仍映射时同步拆私有 mount

堆分配也救不了 `dnsk_cgpids` native: `cgroup_subsys` 回调 (`css_alloc`,
`can_attach`, `fork`, `release`) 是模块代码, 搬 struct 只是移动指针

## cgpids 为什么需要两步卸载

设备内核的 `struct cgroup_subsys` **没有 `owner` 字段**(BTF: 256 字节, 32 个
成员, 止于 `depends_on` 与 `__kabi_reserved_backport1`), cgroup core 不会替
驱动持有模块引用; 仍然注册的 subsystem 必须自持引用, 直到槽位归还
顺序不能反:

1. `quiesce`(module parameter) 还原槽位: subsystem 指针, **每个** `css_set`
 里的 root css, inhibit mask, `have_canfork`/`have_release` 位, static key;
2. 槽位归还后才放下自持引用, refcount 归零, `rmmod` 才能进入 `module_exit()`;
3. 顺序反了就是在 core 仍可能回调时释放模块 text--正是要防的 dangling pointer

`quiesce` 在有任务仍计入模拟控制器时返回 `-EBUSY`.`module_exit()` 不能失败,
所以拒绝必须发生在能失败的 parameter write: `dnskctl unload cgpids` 先写
`/sys/module/dnsk_cgpids/parameters/quiesce`, 成功后才 rmmod.塞进 exit 只会
带着活跃计数释放, 随后 panic

## 卸载前的 in-flight 门控: `dnsk_ns` 的做法

`stop_machine()` 只在还原入口的那一瞬保证没有 CPU 在 hook 里, `synchronize_rcu()`
只排空 RCU 读临界区. 二者都**不会**等待一个睡在模块栈帧里的 task:
它可能阻塞在模块自带的 SysV/POSIX-mqueue syscall 里(`msgrcv`/`semtimedop`/
`mq_timedreceive` 的等待队列), 或停在 `do_exit` 的整段替换体里. 模块 text 一旦
在这类 task 返回前解除映射, 它的 `ret` 就会取指到已释放页(`pc == lr`, `pte=0`,
IABT), 即 2026-11-27 那次真机 panic.

`dnsk_ns` 因此不靠固定 sleep, 而靠一个可观测的**在途计数**:

1. 每个可能睡眠/长时间占用模块 text 的入口(clone/unshare、`copy_namespaces`、
   `alloc_pid`、阻塞型 IPC syscall)先 `ds_ns_enter()`: 取 `try_module_get`
   并 `atomic_inc(&ds_ns_inflight)`, 返回时 `ds_ns_leave()`. 有在途调用时
   模块 refcount > 0, 裸 `rmmod` 直接 `EAGAIN`, 不可能进入 `module_exit()`;
2. 必须执行、不能拒绝的路径(`do_exit`、`free_nsproxy`、`exec_task_namespaces`)
   用 `ds_ns_hold()/ds_ns_unhold()` 只计数不拒绝;
3. `ds_ns_quiesce()`: 置 `ds_ns_quiescing` 挡住所有可拒绝入口 → `synchronize_rcu()`
   → `wait_event_timeout` 等 `ds_ns_inflight` 归零; 只有它为 0 才 `ds_hooks_uninstall()`
   逆序还原并随后 `rcu_barrier()`/`flush_work()`. 还原后所有入口已是内核原生,
   再补一次 `ds_ns_drain()` 收掉极窄窗口里新起的 `do_exit` 之类;
4. `ds_ipc_backend_exit()` 不再对残留 per-task 状态 `WARN_ON`: 这些对象是
   `kzalloc` 堆内存, 卸载后无人再查表, 属于惰性遗留, 只 `pr_info` 报数.

这套顺序由 `tests/host/test_ns_unload_quiesce.py` 守卫: 它建模并发 fork/clone
下 `rmmod` 必须等计数归零, 并断言 `ds_ns_quiesce` 先于 `ds_hooks_uninstall`、
计数入口确实插桩、旧 `WARN` 已移除.

## 新增模块的卸载检查清单

- [ ] 每处 `module_get`/`try_module_get` 都有理由; 能用对称注销解决就不要 pin
- [ ] 每种注册都有对应的逆序注销: hook, feature, `cgroup_subsys`,
 `pernet_operations`, filesystem/superblock, workqueue, timer, RCU 回调,
 task_work, sysfs/kernfs 文件
- [ ] 列出所有可能在 `module_exit()` 之后运行的回调
- 有 → 同步 drain, 或改成
 内核常驻回调, 或自持引用并明确记录不可卸载
- [ ] 比模块活得久的结构不在 `.bss`/`.data`: 移到堆, 函数指针指向内核 text,
 `owner` 保持 NULL, 且 exit 不释放延迟路径仍会用的堆对象
- [ ] 退出路径不包含可能失败的操作; 可失败的拒绝前移到 module parameter 或
 `dnskctl unload` 预检
- [ ] 测试轮询真实 refcount 到 0 取代固定 sleep, 断言卸载后原生态行为恢复;
 ns 探针必须真正创建 namespace(用 `unshare -u -r`; `-f` 只 fork).

## 验证方法

`tests/qemu/init-unload-device` 是窄环验证: core + 单个 feature, 在目标 Image
上走 `dnskctl unload`.它轮询真实 refcount, 只在卸载窗口判定告警, 并断言卸载
后 stock 行为回来

## 相关阅读

- [Hook 机制](hook-mechanism.md): `synchronize_rcu()` 覆盖与不覆盖的范围
- [模块生命周期](module-lifecycle.md): `module_exit` 的逆序清理与 core 引用
- `docs/notes/On-safe-rmmod.md`(本地资料, 不随仓库分发),
 `docs/notes/On-safe-remove-eval.md`(本地资料, 不随仓库分发): 具体故障与修复依据
