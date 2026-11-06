# M0 USER_NS feasibility spike

日期：2026-11-06。上游：`Shikochin/Droidspaces-LKM`
`8c14b35ce353bf1ddab56150733854e3eb440d68`（v0.1.5，GPL-2.0-only）。
目标 KMI：`android16-6.12`（beryl）。状态：**源码可行，集成未验证；不是
Native 证据，不对外宣称。**

## 上游形态

- `src/userns/` 共 43 个文件、约 10.6k 行：`user_namespace.c`、
  `credentials.c`、`utsname.c`、`live-*.c`（cred/ucounts/proc/syscalls/
  mount/fs-context/super/identity/groups/stat/capabilities/fuse/netlink/
  sock/scm/unix/cgroup/time/sysfs/proc-status/nsfs/vfs/exec/mm）、三个
  `*-original.S` 与 compat 头。
- 上游把它编进 **同一个** `droidspaces_ns.ko`：`Makefile` 的
  `TARGET=namespace` 在 `USERNS=1` 时追加全部 live 对象，并定义
  `CONFIG_USER_NS=1`、`DS_USERNS_LIVE=1`、`-include live-names.h`、
  `-I src/userns/compat`。另有 `TARGET=userns` 只构建 `main.c`+
  `user_namespace.c`+`credentials.c`+`utsname.c` 的测试模块，**不含**
  live 替换/VFS/socket/FUSE 路径。
- `src/userns/ORIGIN`：从 Android common kernel 6.12
  `kernel/user_namespace.c`、`kernel/cred.c` 移植，带私有 namespace
  生命周期/workqueue/RCU 排空；`USERNS=1` 时替换 native
  prepare/copy/free 入口与 RCU 回调。

## 依赖分析

对 `src/userns/*.c|h` 的 `ds_*` 引用做交叉比对：userns live 代码大量调用
`src/port` 的定义（hook/kallsyms/ucounts/proc/mount/VFS 辅助），例如
`ds_userns_core_init/live`、`ds_userns_copy_creds`、`ds_un_*ucount*`、
`ds_userns_alloc_mnt_ns`、`ds_userns_begin_new_exec`、
`ds_userns_fuse_*` 等。也就是说：

- 不能把 `src/userns/` 原样建成独立 `dnsk_userns.ko`：这些符号既未导出，
  也不属于 `dnsk_core` 的版本化 API；模块加载器无法解析。
- 若强行复制 `src/port` 进 userns 模块，会与 `dnsk_ns.ko` 重复 syscall
  槽/entry hook/symbol resolver，违反“core 独占 hook”的所有权约束。
- 上游的 `TARGET=userns` 测试模块只能做 namespace 核心/映射/凭据的单元
  验证，不能支撑容器工作负载。

## 可行集成路径

1. **合并构建（推荐先验证）**：在 `dnsk_ns.ko` 增加 `USERNS=1` 构建变体，
   把 port+userns live 编进同一模块；`dnsk_userns.ko` 不作为独立二进制，
   或仅作为只注册 feature/参数的门面。与当前计划的模块边界不一致，需要
   在计划/文档中显式调整，不能悄悄合并。
2. **共享 port 库**：把 `src/port` 中 userns 需要的 hook/kallsyms/ucounts
   辅助收敛为 core 的版本化 API（含 hook 所有权），userns 以独立模块注册；
   工作量大，但保持计划边界。
3. 两者都必须继续实现计划要求的扩展，上游代码不自动满足：
   - idmapped mount（KMI 原生存在时；5.10 等按 KMI 标 N/A）；
   - child user namespace 内 Native OverlayFS；
   - 完整 FUSE 模式（FUSE-BPF backing、no_daemon、fuseblk、passthrough）；
   - kernel selftests/namespaces 全矩阵与 Native oracle 差分。

## 结论

- 源码层可行，但“独立 dnsk_userns.ko”不是一个可直接落地的形态；先做
  合并构建 spike，取得 live userns 在 beryl Image 上的启动/映射/setns
  证据，再决定模块边界调整。
- 在此之前 USER_NS 保持 feasibility gate，不得宣称 Native/Real；M4 的
  idmapped/overlay/FUSE/selftests 仍是发布硬阻塞。
