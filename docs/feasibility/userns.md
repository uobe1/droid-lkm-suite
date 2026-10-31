# USER_NS 可行性门控

## Scope

在 Droidspaces 已有 userns 范围之上，补全并验证：

- uid/gid/project 映射、cred/ucounts、procfs；
- capability/VFS、socket、exec/mm、UTS/mount；
- idmapped mount（该 KMI 存在时）；
- child user namespace 内 Native OverlayFS；
- 完整 FUSE 模式（BPF backing、no_daemon、fuseblk、passthrough 等）；
- Linux kernel selftests/namespaces 全矩阵。

## 事实

- 目标内核 `CONFIG_USER_NS=n`，`/proc/self/ns/user` 不存在。
- Droidspaces `src/userns/` 已实现大量 live 路径并在 PLZ110 验证；但本机 MTK 6.12 未验证。
- 仅 PLZ110 的 BTF/入口指纹不能直接复用于其他设备；每个 KMI 必须重新预检。
- 内联路径（ucount、cred RCU、mount/superblock、FUSE、mm_init 参数）是主要风险。

## 设计

- 完整移植 Droidspaces `src/userns/`，按 `dnsk_` 前缀；所有 hook 由 core 托管。
- 每个 KMI 建立 Native 能力矩阵；上游不存在的特性（如 5.10 的 idmapped mount）标 N/A，不算假成功。
- 所有选中扩展都是硬阻塞；未通过不发布。
- 激活后固定至重启；默认手动加载；boot guard/pstore/safe mode 兜底。

## 验收

- Native oracle：AOSP 同 KMI `CONFIG_USER_NS=y`。
- 差分：文件/syscall/errno/权限/lifetime；unprivileged UID 1000；SELinux Enforcing。
- Droidspaces 全范围测试 + Bubblewrap + 实际 Droidspaces 容器。
- idmapped mount、child overlay、完整 FUSE、kernel selftests 全部通过。
- 失败即项目阻塞，不降级假成功。
