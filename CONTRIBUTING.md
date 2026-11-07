# 贡献规范

## 基本原则

1. **Native 或 Real，无 STUB/FAKE_SUCCESS。** 未实现的功能返回正确 errno 或不注册；禁止显示层、空实现或“看起来成功”。
2. 每个功能必须附带可执行验收证据；源码与文档冲突时以源码和测试结果为准。
3. 不写 boot/vendor_boot/dtbo/system/vendor_dlkm/system_dlkm；不修改 SELinux 为 permissive。
4. 不提交设备私密信息、厂商镜像、密钥或未脱敏日志。

## 分支与提交

- `main` 受保护；所有变更通过 feature 分支 + PR。
- 提交信息使用 Conventional Commits：`feat(scope): ...`、`fix(scope): ...`、`docs(scope): ...`、`test(scope): ...`、`chore(scope): ...`。
- PR 必须通过 CI；合并使用 squash merge。
- 首 tag 前不创建任何 tag；开发版本仅作为构建产物，格式 `vX.Y.ZdevN`。

## 代码风格

- 内核代码：Linux kernel style；`checkpatch.pl --strict`；`.clang-format`；SPDX；英文注释；`dnsk_` 前缀。
- 用户态 C：C11，4 空格，100 列，clang-format。
- Shell：POSIX sh，shellcheck + shfmt。
- Python：ruff。
- Vendored 代码保持上游风格与哈希；适配层按本项目风格。

## 文档

- 中文技术文档 + 英文标识/API；代码注释英文。
- 每篇功能文档必须包含 `Scope`、`Status`、`Non-goals`、`Evidence`。
- 禁止营销语、emoji、无证据的“完整支持/原生等价”表述。
- 本机/环境特定信息（主机名、密钥、容器路径、Termux 本地步骤、个人调试流水）
  不写入 tracked 文档；按三层分流维护：本地 Note（`docs/notes/`，不跟踪）+
  `AGENTS.md` 索引 + Memory 长期事实。tracked 文档只保留通用流程、契约与证据。
- 详见 [docs/STYLE.md](docs/STYLE.md)。

## 许可证

- 内核模块与内核适配代码：GPL-2.0-only。
- 用户态工具、脚本、CI：GPL-3.0-or-later。
- 文档：CC-BY-SA-4.0。
- 新增文件必须带对应 SPDX 头；vendored 代码保留原 SPDX。
