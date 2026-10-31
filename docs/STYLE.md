# 文档规范

## 语言与术语

- 正文默认中文；代码标识、配置名、API、错误码保持英文。
- 代码注释英文；内核 API 用 kernel-doc 格式，用户态 C 用 Doxygen 格式。
- 同一概念使用固定术语：`feature`、`module`、`native`、`real`、`differential`、`virtual`、`N/A`、`unsupported`、`not-shipped`。
- 不使用营销语、夸张承诺、emoji；结论必须能追溯到源码、测试或日志。

## Markdown 规则

- CommonMark；ATX 标题；标题层级不跳级。
- 围栏代码块必须标注语言；路径/命令使用行内代码。
- 表格用于状态矩阵、验收矩阵、版本固定；避免超宽表格。
- 图使用 Mermaid；禁止直接嵌入不可审计的外部图片。
- 由 markdownlint 检查；链接必须可解析。

## 页面结构

每个功能页必须包含：

```text
# 功能名
## Scope
## Status
## Design
## Non-goals
## Evidence
```

- `Status` 使用 N/R/D/V/N/A/unsupported/not-shipped，并注明 KMI。
- `Evidence` 链接到测试日志、CI run、差分报告或真机记录；没有证据就写 `pending`，不得写“已验证”。
- 变更历史不写入功能页；由 Git 和 release notes 记录。

## 生成文档

- 内核 API：kernel-doc（`scripts/kernel-doc`）生成，放入 `site/api/kernel/`。
- 用户态 C API：Doxygen 生成，放入 `site/api/userspace/`。
- 导航与指南：MkDocs Material；版本化使用 mike 发布 GitHub Pages。
- 生成物不手工编辑；注释是唯一 API 文档来源。

## 许可证

文档文件使用 CC-BY-SA-4.0；若嵌入代码片段，片段沿用对应代码许可证。
