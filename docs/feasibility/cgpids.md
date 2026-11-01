# CGROUP_PIDS 可行性结论 (android16-6.12)

状态: 可行性研究完成, 未实现. 本页是设计门控, 不是已验证能力.

## 结论

- **cgroup v2 虚拟 pids 控制器**可以在不注册真实 `cgroup_subsys` 的前提下达到
  Docker 29.x `--pids-limit` 契约: 真实层级计数、真实 `-EAGAIN`、真实
  `pids.max/current` 文件、真实迁移计数.
- **cgroup v1 的未修改 Docker `--pids-limit` 不可实现**: `mount -o pids`
  需要编译期存在的 `SUBSYS(pids)`; 运行时注册不存在, 因此 v1 无法获得
  Docker sysinfo 认可的 pids mount.
- 6.12 原生 pids 文件是 `pids.max/current/peak/events[/.local]`;
  `pids.stats` 不存在, 不应伪造.
- 该结论会影响计划中 "Docker v1 与 v2 都必须无修改通过" 的发布门控:
  v1 只能明确标为 unsupported, 或使用 Native AOSP 内核; 不能假装支持.

## 关键机制

### cftype 与 Docker

`cgroup_add_dfl_cftypes()`/`_legacy_cftypes()` 导出了, 但都依赖已存在的
`cgroup_subsys` 与 `ss->root`; 把 pids 文件挂到 memory/cpu subsys 会因
v1/v2 绑定与可见性产生错误语义. v2 更稳的方式是:

- `cgroup_init_cftypes(NULL, cfts)` 初始化 cftype 数组;
- 在导出 `cgroup_mutex` 下用 `cgroup_addrm_files(&cgrp->self, cgrp, cfts, add)`
  直接把文件加到目标 cgroup. `ss == NULL` 时 `of_css()` 返回
  `&cgrp->self`, 回调可拿到 cgroup.
- 不要对未链接的数组调用 `cgroup_rm_cftypes()`.

Docker 29 v2 读取 `cgroup.controllers` 判断 pids 可用, runc 写
`+pids` 到 `cgroup.subtree_control`, 然后写 `pids.max`; sysinfo 在 dockerd
启动时缓存, 因此模块必须先于 dockerd 加载.

### fork 强制

- 原生 pids 在 `can_fork` 中沿祖先链原子累加, 失败回滚; `cancel_fork`/
  `release` 撤销后续失败与退出.
- 不能用 kretprobe 改 `cgroup_can_fork` 返回值: 函数已持有 css_set 引用,
  fork 失败路径会跳过 `cgroup_cancel_fork`, 泄漏引用.
- 推荐劫持现有 subsys callback 槽 (`have_canfork_callback` 与
  `memory_cgrp_subsys.can_fork/cancel_fork/release`), 它运行在
  `cgroup_css_set_fork` 之后、`cgroup_threadgroup_rwsem` 下, `cset->dfl_cgrp`
  稳定, 与 Native 上下文一致. 必须在 preflight 证明没有其他 subsys 占用
  can_fork, 记录原指针/掩码, 卸载时恢复.
- 纯 deny-only 才可用 `cgroup_can_fork` 入口 PC rewrite 直接返回 `-EAGAIN`;
  不能在入口做 charge (入口时 `kargs->cset` 未设置且父进程可迁移).

### v2 委派

`cgroup.controllers` 由 u16 mask 打印, mask 不可能出现 "pids" 位. 必须
hook `cgroup_controllers_show`/`cgroup_subtree_control_show` 追加 pids,
并在 `cgroup_subtree_control_write` 中把 `+pids` 从内核解析前剥离并记录
fake enabled 状态. 不能把 `+pids` 翻译成 `+memory`/`+misc` (会泄漏 controller,
改变 domain/threaded 与 no-internal-process 规则).

### 迁移与生命周期

- hook `cgroup_migrate_execute` 入口: taskset 已在 `cgroup_mutex` +
  threadgroup 写下固定; 逐任务从 `task_css_set(task)->dfl_cgrp` 计数移到
  `mg_dst_cset->dfl_cgrp`; 用 kretprobe 在返回非零时回滚.
- rmdir 在有任务时本就是 `-EBUSY`; 计数状态用 kref/RCU 生命周期, 不能
  仅靠 rmdir 删除, 否则失败 rmdir 会丢失限制.
- threaded cgroup、CLONE_INTO_CGROUP、nsdelegate 等未验证场景 fail-closed.

## 设计轮廓

- 状态: 以 `struct cgroup *` 为键的 xarray; 每项
  `atomic64_t counter/limit/peak/events[2]/events_local[2]`, fake enable mask,
  kref + RCU; 祖先链用 `cgrp->self.parent`.
- 文件: 真实 cftype 文件 (`pids.max/current/peak/events`), 回调读写 xarray.
- hook: subsys fork/release 回调, `cgroup_migrate_execute` ± return,
  controllers/subtree show 与 subtree_control_write, mkdir/destroy.
- 现有 core hook 只有 kprobe pre-handler 与 data patch; 需补 kretprobe/
  return hook 与受保护的 callback 槽 patch.
- preflight: `CONFIG_CGROUP_PIDS=n`、v2 mount、符号/静态地址存在、callback
  槽为空、`init_css_set.dfl_cgrp` 正确、hook 命中计数; 任一失败不出现
  `pids` 文件/controllers, 即 fail-closed.

## 不支持的语义

- v1 pids mount 与 Docker v1 `--pids-limit`;
- `/proc/cgroups`/subsys ID/css 枚举中的 pids;
- `pids.stats` (6.12 本就不存在);
- 未验证的 threaded/CLONE_INTO_CGROUP/nsdelegate 组合;
- KCFI 签名不匹配的 callback 槽.

## 先例

pmb110 (`ai-luo2006/droidspaces-6.12-pmb110`) 使用显示层 inline hook、
kprobe `can_fork` PC-return、post_fork 计数与 `+pids`→`+memory`; 已知问题:
trampoline BRK 导致写 cgroup.procs 硬崩、迁移任务不计、post_fork 并发超限、
`pids.max=0` 语义错误、rmdir 失败丢限制. 本项目不重复这些做法.
