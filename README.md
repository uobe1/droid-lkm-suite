# droid-lkm-suite

为已 Root 的 KernelSU 设备补全 Android GKI 缺失的容器内核能力: 按需加载 `.ko`,
不写 boot 分区, 不重编内核

## 这是什么

一台 Android GKI 设备的内核配置里常常关掉了容器需要的功能 (`CONFIG_USER_NS`,
`CONFIG_PID_NS`, `CONFIG_CGROUP_PIDS`, `CONFIG_TMPFS_XATTR` 等).
这个项目把这些功能以可加载模块的形式补回去, 让 Docker, Droidspaces 一类的容器
能在原厂内核上跑起来

加载后, **内核自己执行这些语义** -- 不是模块在旁路模拟
这一区别是本项目的
硬约束, 也是它区分于"hook 一下看起来能用"的地方

```sh
# 典型用法
dnskctl probe            # 看看这台设备缺什么
dnskctl load ns          # 加载一个 feature
dnskctl monitor          # 实时看各模块状态
dnskctl start-all        # 或者一次全开
```

## 实现分两类

理解这个项目只需要知道一件事: 加载后, **内核态和用户态两个层面**的语义是否
都与原生相同

| 类别 | 含义 | 判据 |
|---|---|---|
| **O** (Native/Origin) | **内核态与用户态语义都与原生相同** | 换成"打开该 CONFIG 重编的内核", 从 `/proc`, `/sys`, 结构布局到 errno, 计数, 都看不出区别 |
| **V** (Visual/Only-feature) | **仅用户态语义相同**(或用户态也只部分相同) | 用户态看着一样, 但内核里没有真实子系统: `/proc/cgroups` 无该行, controller ID 不同, 计数由模块自己维护 |

当前 `ns`, `devtmpfs`, `tmpfsx`, `cgpids`(native 后端), `ipset`, `netfilter`
都是 **O**; 只有 `cgpids` 的 `native=0` 虚拟控制器是 **V**(用于无法占用 pids 槽位的内核).

分类规则与逐项依据见 `docs/notes/On-OV-split.md`

## 状态

当前为 **android16-6.12 开发阶段**, 只产出开发工件, 不打 tag

- 已在真机 (beryl, `6.12.30-android16-5`) 加载并通过功能测试
- 已与原生 GKI 内核做过语义差分: 用户态与内核态观测一致
- 每个模块都能安全卸载, 卸载后内核行为回到原生态

逐项状态与证据: [docs/status.md](docs/status.md).

## 文档

| 想了解 | 看这里 |
|---|---|
| 架构与模块划分 | [docs/architecture/overview.md](docs/architecture/overview.md) |
| 当前功能状态与证据 | [docs/status.md](docs/status.md) |
| 构建与真机加载步骤 | [docs/guides/build.md](docs/guides/build.md), [docs/guides/device.md](docs/guides/device.md) |
| API 与 ABI | [docs/api/](docs/api/) |
| 全部文档索引 | [docs/index.md](docs/index.md) |

## 构建

大体积内核构建不在本地做, 由 GitHub Actions + DDK 完成; 本地只做静态检查与真机
加载验证

```sh
make KDIR=/path/to/kernel M="$PWD" modules
```

## 许可证

按文件类型分层授权:

- 内核模块与内核适配代码: GPL-2.0-only, 见 [LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt)
- 用户态工具, 脚本, CI: GPL-3.0-or-later, 见 [LICENSES/GPL-3.0-or-later.txt](LICENSES/GPL-3.0-or-later.txt)
- 文档: CC-BY-SA-4.0, 见 [LICENSES/CC-BY-SA-4.0.txt](LICENSES/CC-BY-SA-4.0.txt)
- Vendored 上游代码保留其原始 SPDX 与许可证

作者: Uobe(uobe1).

---

维护者须知 (范围约束, 验收标准, 提交规则) 见 [CONTRIBUTING.md](CONTRIBUTING.md).
