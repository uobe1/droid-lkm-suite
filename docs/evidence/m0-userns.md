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

## 2026-11-06 实现进展

- 已把上游 `src/userns/*`（43 文件）vendor 到
  `features/ns/vendor/userns/`，新增 `DNSK_USERNS=1` 构建开关，把
  `user_namespace.c`、`credentials.c`、`utsname.c` 与全部 `live-*.c`、
  `*-original.S` 编入同一个 `dnsk_ns.ko`；`port/main.c` 的
  `DS_USERNS_LIVE` 初始化路径随之启用。
- 未导出符号继续由 `ksud insmod` 解析；`kallrecon` 不参与。
- `ci-fast` 增加 `userns=true` 输入构建该 spike；设备镜像门
  `tests/qemu/init-userns-device` + `userns_test.c` 已就绪，验证
  uid/gid 映射、嵌套 userns 与 boot_id 不变。
- 仍未验证：编译通过、idmapped mount、child-userns OverlayFS、完整
  FUSE 模式、kernel selftests 全矩阵。不得据此宣称 D/N。

## 2026-11-06 首批设备门结果

- 合并构建（ci-fast `userns=true`，run `42247295351`）成功；但设备 Image
  门 run `42248410082` 在 `ksud insmod dnsk_ns.ko` 时失败：
  `Unknown symbol security_path_chroot`、`security_path_chown`、
  `__tracepoint_android_vh_mmap_lock_free`、
  `__traceiter_android_vh_mmap_lock_free`。
- 这些符号在 beryl 内核中不存在（`CONFIG_SECURITY_PATH` 相关 LSM 钩子未编
  入、厂商 tracepoint 未定义），ksud 无法解析不存在的符号。
- 适配：`vendor/ksud/symbols.c` 增加运行时可选解析；
  `security_path_chroot/chown` 缺失时按原生未启用路径钩子返回 0，
  vendor traceiter 缺失时跳过。此类“内核没有的符号”不得写死 import，
  也不能在模块里伪造实现。

## 结论

- 源码层可行，但“独立 dnsk_userns.ko”不是一个可直接落地的形态；先做
  合并构建 spike，取得 live userns 在 beryl Image 上的启动/映射/setns
  证据，再决定模块边界调整。
- 在此之前 USER_NS 保持 feasibility gate，不得宣称 Native/Real；M4 的
  idmapped/overlay/FUSE/selftests 仍是发布硬阻塞。

## 设备 Image 首门与 ThinLTO 修正（2026-11-06）

合并构建 `dnsk_ns.ko`（`DNSK_USERNS=1`）在 beryl Image 上经 ksud 加载：
PID/IPC preflight、分配/共享/延迟销毁与 `DNSK_USERNS_LKM_OK` 通过。
`unshare(CLONE_NEWUSER)` 返回 `EINVAL`：该 GKI 用 ThinLTO 把
`ksys_unshare()` 内联进 `__arm64_sys_unshare`，只 hook `ksys_unshare`
不会命中 syscall 路径，调用落入 `CONFIG_USER_NS=n` 的 stub。

修正：同时 hook `__arm64_sys_unshare` 到 `ds_unshare` 包装
（`ds_ksys_unshare`），并在 live 初始化前用核心 kallsyms 检查原生
`create_user_ns`；存在原生 USER_NS 时拒绝加载，保留原生实现。门重跑中。

## 基础设备门通过（2026-11-06，run 42255695799）

`__arm64_sys_unshare` 入口 hook 生效后，beryl Image 上：

- preflight：111 functions / 39 syscalls / 7 callbacks；
- PID/IPC 自检与模块加载通过（`DNSK_USERNS_LKM_OK`）；
- `unshare(CLONE_NEWUSER)` + `uid_map`/`gid_map` + 嵌套 userns 通过
  （`USERNS_TEST_OK uid=0 gid=0`，`DNSK_USERNS_OK`）。

门内同时检查原生 USER_NS：`create_user_ns` 在核心 kallsyms 中存在时拒绝
加载，保留原生实现。

## idmapped mount 的 !USER_NS 适配（开发中）

`fs/namespace.c:build_mount_idmapped()` 在 `CONFIG_USER_NS=n` 下从
`userns_fd` 解析出的命名空间会被内联 `get_user_ns()` 桩替换成
`init_user_ns`，随后 `alloc_mnt_idmap(init)` 复制全等映射；同时
`can_idmap_mount()` 会因 `mnt_userns == sb->s_user_ns` 返回 `EINVAL`。

适配：
- hook `__arm64_sys_mount_setattr`：进入时解析 `userns_fd` 对应命名空间，
  按任务暂存一个引用，再走原生 trampoline；
- hook `alloc_mnt_idmap`：当内核传入 `init_user_ns` 且当前任务有暂存时，
  用真实命名空间复制 uid/gid map；调用返回后释放暂存引用；
- `mount_setattr` 测试改为：helper A（映射 0→kuid 1）在自己的 mount ns
  挂 tmpfs 并创建文件，init 通过 `setns` 进入；idmap 使用 helper B 的
  命名空间，使文件 uid 从 1 变为 0。

原生 USER_NS 内核仍在加载时直接拒绝，不启用这些 hook。
