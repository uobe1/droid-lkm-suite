# CGROUP_PIDS 可行性门控

## Scope

在目标内核 `CONFIG_CGROUP_PIDS=n` 上，为 Docker 29.x cgroup v1/v2 提供可用的 `--pids-limit`，并保证 Native vs Virtual 差分契约一致。

## 事实

`include/linux/cgroup_subsys.h`：

```c
#if IS_ENABLED(CONFIG_CGROUP_PIDS)
SUBSYS(pids)
#endif
```

当 `CONFIG_CGROUP_PIDS=n`：

- `pids_cgrp_id` 不存在；
- `CGROUP_SUBSYS_COUNT` 少一项；
- `cgroup_subsys[]` 无 pids 槽；
- `css_set.subsys[]`、`cgroup.e_csets[]` 均为编译期定长。

本机 `/proc/cgroups` 只有 8 个控制器：cpuset/cpu/cpuacct/blkio/memory/freezer/net_cls/net_prio。

`kernel/cgroup/cgroup.c` 的 `cgroup_init_subsys()` 是 `static __init`，启动后释放，且 `BUG_ON(!list_empty(&init_task.tasks))`；`cgroup_init()` 只在 boot 期无任务时运行。没有 runtime `register_cgroup_subsys()` API。

结论：**原生 pids 子系统无法通过 LKM 安全注册**。任何“显示 pids controller 但不改变 cgroup 核心布局”的方案都是假成功，禁止。

## 设计

采用**虚拟 pids 控制器**：

- 私有 xarray 按 `struct cgroup *` 保存 `counter/limit/events`；
- 中心 hook：`cgroup_can_fork`/`cancel_fork`、`cgroup_migrate`/`attach_task`、`cgroup_exit`/release、`cgroup_mkdir`/`rmdir`、`cgroup_controllers_show`、`proc_cgroupstats_show`、`cgroup_subtree_control_write`；
- 通过 `cgroup_add_dfl_cftypes`/`legacy_cftypes` 在 carrier controller 上创建真实 `pids.max/current/events` kernfs 文件；
- 真实层级计数、迁移计入、超限 `-EAGAIN`；对象生命周期与 cgroup 删除/reparent 并发处理；
- 不做 pure display layer；若任何中心 hook 或路径无法覆盖，fail-closed，不暴露 pids 文件。

## 契约内

- `pids.max/current/events` 读写/errno；
- 层级计数；
- fork/clone 超限；
- 迁移/回滚；
- mkdir/rmdir/reparent；
- 并发 fork + migrate + rmdir。

## 范围外

- threaded cgroup；
- 独立 v1 pids mount；
- css/subsys 枚举/ID。

范围外必须返回 EOPNOTSUPP 或文档标 unsupported，不伪造。

## 验收

- Native oracle：AOSP 同 KMI `CONFIG_CGROUP_PIDS=y`。
- 同一 rootfs/测试套件，逐个比较文件内容/errno/计数/fork 结果/迁移/删除/竞态。
- Docker 29.x cgroup v1/v2 `--pids-limit` 无修改可用。
- 任一项差异导致项目阻塞；不得用“显示层通过”替代。
