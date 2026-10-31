# droid-lkm-suite

Android GKI 容器子系统补全的 LKM 套件：在不写 boot 分区、不重编内核的前提下，为已 Root 的 KernelSU 设备按需加载 `.ko`。

本项目以“**Native 或 Real，无 STUB/FAKE_SUCCESS**”为硬约束。当前处于 **M0 工程骨架阶段**：仓库、core ABI、hook/patch 基础、preflight、KSU/CI/文档管线正在建立；任何功能在通过对应验收前都不会被标记为支持。

## 范围

- 目标：Android GKI arm64、4 KiB 页、KMI `android12-5.10` 至 `android17-6.18`。
- 设备验证基线：`android16-6.12`；首 tag 需全部 8 个 KMI 与全部目标功能通过，未完成不打 tag。
- 运行方式：`dnsk_core.ko` 常驻，feature `.ko` 由 `dnskctl`/KSU WebUI 通过 `ksud insmod` 按需加载。
- 禁止：写 `/boot`、`vendor_boot`、`dtbo`、`system`、`vendor_dlkm`、`system_dlkm`；使用 `/system/bin/insmod`；伪造 controller、文件或“成功”状态。

## 文档与状态

- 文档索引：[docs/index.md](docs/index.md)
- 文档规范：[docs/STYLE.md](docs/STYLE.md)
- 当前状态：[docs/status.md](docs/status.md)
- 架构说明：[docs/architecture/overview.md](docs/architecture/overview.md)
- M0 可行性门控：[docs/feasibility/](docs/feasibility/)

状态等级：

| 等级 | 含义 |
|---|---|
| N | Native：宿主已具备，或在完整验收矩阵内与 AOSP Native oracle 一致 |
| R | Real source-equivalent：同 KMI 源码移植，源码差异可审计，功能/容器工作负载通过 |
| D | Differential parity：与 AOSP Native oracle 在定义契约内一致 |
| V | Virtual controller：真实状态/强制/文件，但非内核原生 `cgroup_subsys` |
| N/A | 该 KMI 上游不存在此特性，不属于该 KMI 验收项 |

任何未达到 N/D/R 的功能都会在 status 中明确标为 `unsupported` 或 `not-shipped`，不会以空实现或显示层替代。

## 构建

本仓库不在本地进行大体积内核构建。构建由 GitHub Actions 的标准 x86_64 runner + DDK 完成，QEMU 使用 TCG；本地只做静态检查与最终真机加载/smoke。

```sh
make KDIR=/path/to/kernel M="$PWD" modules
```

CI 与真机命令见 [docs/guides/build.md](docs/guides/build.md) 与 [docs/guides/device.md](docs/guides/device.md)。

## 许可证

本项目按文件类型分层授权：

- 内核模块与内核适配代码：GPL-2.0-only，见 [LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt)。
- 用户态工具、脚本、CI：GPL-3.0-or-later，见 [LICENSES/GPL-3.0-or-later.txt](LICENSES/GPL-3.0-or-later.txt)。
- 文档：CC-BY-SA-4.0，见 [LICENSES/CC-BY-SA-4.0.txt](LICENSES/CC-BY-SA-4.0.txt)。
- Vendored 上游代码保留其原始 SPDX 与许可证；不整体 reformat。

作者：Uobe（uobe1）。
