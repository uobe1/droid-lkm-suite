# KSU 打包与安装证据

日期：2026-11-07。范围：dev KSU ZIP 的构建、结构校验与真机安装/卸载语义。
不做 tag；产物为 `v1.0.0dev0`。

## 构建

`package.yml`（复用 `ci-fast` 的 `core-android16-6.12` 构件）：

| 运行 | 用途 | 结论 |
|---|---|---|
| `42379143397` | 首次打包 | success（ZIP 结构校验通过） |
| `42379368343` | 修复 customize.sh `set -e` 后重打 | success |
| `42379496534` | 修复 MODPATH 解析后重打 | success |

最新 artifact：`droid-lkm-suite-dev` id `13577330744`，模块 ZIP
`droid_lkm_suite-dev.zip`（26 项）。解包后的外层 artifact 内含模块 ZIP。

## 结构校验

- `manifest.json` 的 24 项 `sha256`/`size` 全部一致；`verification.json`
  为 `dev=true`、`tagged=false`。
- 包含 `module.prop`、安装/启动/卸载脚本、`bin/{dnskctl,dnsk-preflight,
  panic_decode.sh}`、`kmi/android16-6.12/*.ko`、`webroot/*`、`policy.conf`、
  `supported_kmis.txt`。
- `webroot/app.js` 通过 `node --check`；`bin/dnskctl` 与仓库版本逐字节一致；
  `bin/dnsk-preflight` 为 aarch64 static。

## 真机安装（`ksud module install`）

设备内核 `6.12.30-android16-5-g1ed949324a3e-ab13881345-4k`，KMI
`android16-6.12`，ksud `3.3.1`，存在 metamodule `hybrid_mount`。

- 首次安装失败：`Failed to install module script`。差分定位为
  `customize.sh` 被安装器 source 后 `set -e` 泄漏（见
  `docs/notes/On-ksu-packaging.md`，提交 `83ada71`）。
- 修复后安装成功，但 `bin/*` 权限为 0644（解压不保留执行位）；
  根因是被 source 后 `${0%/*}` 不是模块路径。改用 `MODPATH` 后
  （提交 `4e2070d`）安装产物 `bin/dnskctl`、`bin/dnsk-preflight`、
  `*.sh` 均为 0755。
- 安装后 `ksud module list` 显示模块处于 update 状态，文件在
  `/data/adb/modules_update/<id>/`，下次启动生效。
- 安装后 CLI 校验：`dnskctl probe` / `status` / `preflight core`
  均 rc=0；`post-fs-data.sh` 能解析模块路径并调用 `ksud insmod`
  （现场 core 已加载，故返回 `File exists`；全新启动路径需重启验证）。

## 卸载语义

- `ksud module uninstall <id>` 返回 0 但仅打 `remove` 标记；模块目录在
  下次启动时物理删除并执行 `uninstall.sh`（状态目录按设计保留）。
- 因此“自动卸载”不是纯热操作：pin 住的模块（ns 等）与物理删除都需要
  一次重启；可热卸载的 feature 用 `dnskctl unload`（见 status）。
