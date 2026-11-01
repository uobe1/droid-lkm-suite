# dnsk_ns pidfd setns panic (2026-11-01)

## 现场

设备 boot `47d5f50c-b448-4bd9-b69e-0634812213dc`，最新 pstore
`console-ramoops-0` (12:06)：

```text
Unable to handle kernel NULL pointer dereference at virtual address 00000000000004e0
Internal error: Oops: 0000000096000005 [#1] PREEMPT SMP
pc : __arm64_sys_ds_setns+0x414/0x704 [dnsk_ns]
lr : __arm64_sys_ds_setns+0x3c4/0x704 [dnsk_ns]
CPU: 7 UID: 0 PID: 29868 Comm: nsenter
Call trace:
 __arm64_sys_ds_setns+0x414/0x704 [dnsk_ns]
```

反汇编 + debug line 定位到 `features/ns/vendor/port/nsproxy.c:438`
（`validate_nsset()` pidfd 路径）：

```c
ret = validate_ns(nsset, &nsp->ipc_ns->ns);
```

`x8 = nsp->ipc_ns = 0`，随后 `ldr x9, [x8, #1248]`（`ipc_ns->ns.ops`）
触发 NULL deref。触发进程是 `nsenter`，走的是 pidfd + `CLONE_NEWIPC` 路径。

## 根因

目标设备内核未开启 `CONFIG_IPC_NS`/`CONFIG_POSIX_MQUEUE`，`init_nsproxy.ipc_ns`
及模块加载前创建的旧任务 `nsproxy->ipc_ns` 均为 NULL。dnsk_ns 用 DDK 头编译，
DDK 开启 IPC_NS，因此 `validate_nsset()` 无条件解引用该字段。内核自带代码在
IPC_NS=n 时把这段编译掉，所以只有我们的模块踩到。

## 修复

- `validate_nsset()` 对 mnt/uts/ipc/cgroup/net/time 的目标指针全部先判 NULL，
  缺失时返回 `-EINVAL`（fail-closed，不再解引用）。
- `put_ipc_ns()`、`put_pid_ns()` 接受 NULL；`pidns_install()` 对旧
  `pid_ns_for_children` 判 NULL；`commit_nsset()` 的 timens 提交判 NULL。
- 回归测试 `tests/device/setns-pidfd-smoke.c`：`pidfd_open(1)` +
  `setns(fd, CLONE_NEWIPC)`，期望 `-EINVAL` 且不崩溃。

## 修复与复测 (2026-11-01)

CI run `41272845539` 构建通过（commit `75991e5`）。设备 boot
`47d5f50c-b448-4bd9-b69e-0634812213dc`：

```text
pidfd_open(1) + setns(pidfd, CLONE_NEWIPC)
  -> ret=-1 errno=22 (Invalid argument)   # 不再 panic
run-ns-smoke.sh -> NS_SMOKE_OK, namespace_cycles=100
droidspaces_ns: entry preflight passed: 21 functions, 20 syscalls, 6 callbacks
```

`boot_id` 未变，无 Oops/BUG/panic；`dnsk_ns` 与 core 正常加载并 pin。
