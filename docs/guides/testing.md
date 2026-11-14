# 测试策略

## 分层

| 层 | 内容 |
|---|---|
| unit | 用户态工具, KMI 解析, 打包, panic 解析, 策略 |
| host | dnskctl/boot guard/preflight 负例, KSU ZIP 结构 |
| qemu | 模块加载/卸载, 功能, 崩溃扫描, Native vs LKM 差分 |
| device | ksud 加载, ns/ipc/devtmpfs smoke, 已有 droidspaces 容器, pstore 取证 |
| differential | TMPFS/USER_NS/CGROUP_PIDS 与 AOSP Native oracle |
| bench | fork/clone, unshare/setns, SysV/mqueue, nft/ipset, tmpfs xattr/ACL, userns |

## 规则

- 没有 Native oracle 的差分结论无效
- 任何 `panic/Oops/BUG/CFI failure/refcount_t/Unknown symbol` 即失败
- 未通过验收的功能保持 not-shipped; 不允许以 smoke 通过代替契约差分
- 单 job 目标约 20 分钟; 长测试在 Codespaces 或分片 matrix 中运行
