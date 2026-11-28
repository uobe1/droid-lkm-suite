# 贡献规范

## 基本原则

1. **Native 或 Real, 无 STUB/FAKE_SUCCESS.** 未实现的功能返回正确 errno 或不注册; 禁止显示层, 空实现或"看起来成功"
2. 每个功能必须附带可执行验收证据; 源码与文档冲突时以源码和测试结果为准
3. 不写 boot/vendor_boot/dtbo/system/vendor_dlkm/system_dlkm; 不修改 SELinux 为 permissive
4. 不提交设备私密信息, 厂商镜像, 密钥或未脱敏日志

## 分支与提交

- `main` 受保护; 所有变更通过 feature 分支 + PR
- 提交信息使用 Conventional Commits: `feat(scope): ...`, `fix(scope): ...`, `docs(scope): ...`, `test(scope): ...`, `chore(scope): ...`
- PR 必须通过 CI; 合并使用 squash merge
- 首 tag 前不创建任何 tag; 开发版本仅作为构建产物, 格式 `vX.Y.ZdevN`

## 代码风格

- 内核代码: Linux kernel style, 由 `.clang-format` 在 CI 强制; SPDX; 英文注释; `dnsk_` 前缀
- 用户态 C: C11, tab 缩进, 100 列, clang-format
- Shell: POSIX sh, shellcheck 在 CI 强制
- Python: ruff 在 CI 强制
- `make check` 运行 API 文档漂移, tracked Markdown 风格, workflow 输入一致性,
  host 单测, 以及与 CI 同一套 lint 面: shellcheck, ruff, clang-format, yamllint,
  actionlint, cppcheck; 本地缺少某个工具时该面输出 `UNAVAILABLE` 并让门控以
  退出码 2 结束 (incomplete), 不声明已检查
- 新增公开声明必须同时写 kernel-doc 或 Doxygen 注释, 否则 `make check` 报错
- Vendored 代码保持上游风格与哈希; 适配层按本项目风格

## 文档

- 中文技术文档 + 英文标识/API; 代码注释英文
- 每篇功能文档建议包含 `Scope`, `Status`, `Non-goals`, `Evidence`; 结构服务于内容, 不强制模板
- 禁止营销语, emoji, 无证据的"完整支持/原生等价"表述
- 本机/环境特定信息 (主机名, 密钥, 容器路径, Termux 本地步骤, 个人调试流水)
 不写入 tracked 文档; 按三层分流维护: 本地 Note(`docs/notes/`, 不跟踪)+
 `AGENTS.md` 索引 + Memory 长期事实; tracked 文档只保留通用流程, 契约与证据
- 详见 [docs/STYLE.md](docs/STYLE.md).

## 许可证

- 内核模块与内核适配代码: GPL-2.0-only
- 用户态工具, 脚本, CI: GPL-3.0-only
- 文档: CC-BY-SA-4.0.
- 新增文件必须带对应 SPDX 头; vendored 代码保留原 SPDX
