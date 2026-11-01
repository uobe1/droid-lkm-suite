# M1 namespace evidence (android16-6.12)

日期：2026-10-31。范围：`dnsk_ns.ko`，vendored Droidspaces-LKM 6.12 PID/IPC 端口；仅
`android16-6.12` 构建。没有 tag，没有 release 声明。

## Artifact

`droid_lkm_suite-v1.0.0dev0-android16-6.12.zip`
sha256 `418b86b56d5e5066a70f78778749b482bd5fa1e51494dc9be9b48999d8b567a3`
（core + ns；CI run `41234788684`）。

## activate=0（低风险自测）

```text
Loaded kernel module: .../dnsk_ns.ko
dnsk_ns 315392 0
dnsk_core 45056 0
droidspaces_ns: PID: allocation, parent, IDR, sharing and invalid-parent checks passed
droidspaces_ns: IPC: allocation, namespace identity, owner and sharing checks passed
droidspaces_ns: PID/IPC: deferred destruction finished, no live namespaces
droidspaces_ns: namespace core selftests passed; syscall hooks are not installed
```

`rmmod dnsk_ns` 和 `rmmod dnsk_core` 均成功。

## activate=1（真实 hooks）

```text
droidspaces_ns: entry preflight passed: 21 functions, 20 syscalls, 6 callbacks; no patches written
droidspaces_ns: experimental namespace hooks active; module stays pinned until reboot
/etc:
ipc -> ipc:[4026531839]
pid -> pid:[4026531836]
pid_for_children -> pid:[4026531836]
```

### PID namespace

在 namespace 内执行独立脚本：

```text
child_pid=1
child_pidns=pid:[4026535021]
child_pid_for_children=pid:[4026535021]
proc1=Name: sh
proc_count=3
unshare_pid_rc=0
```

nested PID namespace：

```text
child_pid=1
child_pidns=pid:[4026535023]
nested_rc=0
```

100 次 `unshare -p -f /system/bin/true` 循环：`loops=100 fail=0`。

### IPC namespace + SysV

```text
parent: ipcns ipc:[4026531839], msgget 0, msgsnd 0, msgrcv 5 "hello", msgctl_rmid 0
ipcns:  ipc:[4026535020], same queue ops
pid+ipc: pid 1, ipc:[4026535021], same queue ops
```

### POSIX mqueue

通过 raw syscall 测试；注意 kernel ABI 的 name 不带前导 `/`，接收 buffer 必须 >=
默认 `mq_msgsize` (8192)。父进程、IPC ns、PID+IPC ns 均通过：

```text
parent:  ipc:[4026531839] mq_open 4, send 0, receive 5 "hello" prio 1, unlink 0
ipcns:   ipc:[4026535013] same
pid+ipc: pid 1 ipc:[4026535020] same
```

## 2026-11-01 复测 (boot_id `aa1f26d0-...`)

使用 CI run `41270878281` 的 6.12 build，当前代码无 ns 源改动：

```text
ksud insmod dnsk_ns.ko gki=1 activate=1 -> rc=0, refcnt=1
droidspaces_ns: entry preflight passed: 21 functions, 20 syscalls, 6 callbacks
droidspaces_ns: experimental namespace hooks active; module stays pinned until reboot
run-ns-smoke.sh:
  pid-ns / nested-pid-ns / ipc-ns / pid-ipc-ns all pass
  mqueue-parent / mqueue-ipc / mqueue-pid-ipc all pass
  namespace_cycles=100
  NS_SMOKE_OK
```

`boot_id` 未变，无 Oops/BUG/panic。core 在 ns/devtmpfs 激活后 refcnt=1。

组合容器 smoke：在新 PID+mount namespace 内挂载 devtmpfs，`mini_dev_ok`，
PID ns `pid:[4026535006]`，退出后宿主无残留 mount。droid-distro Debian
容器内嵌套容器同样通过（`NESTED_DEV_OK`，PID 1 为 sh），宿主 `/dev` 未变。

## 已知边界

- 模块激活后 pinned；替换需重启。
- 未测试：setns 到已有 ns、container reboot、100 容器、Docker、QEMU ns。
- nested PID 已由 `run-ns-smoke.sh` 覆盖。
- 不得据此创建 tag 或宣称完整 PID/IPC 支持。
