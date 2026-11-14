# dnskctl 命令手册

设备侧唯一受支持的用户态入口
所有命令读取真实系统状态, 不缓冲, 不美化

约定:

- `--json` 输出单行 JSON 对象; 退出码仍然反映错误
- 正常结果到 stdout, 错误到 stderr
- 除 `probe`/`cap`/`status` 这类只读命令外, 其余都会在动手前跑
 `dnsk-preflight`; preflight 失败则拒绝继续

## 探测与状态

| 命令 | 作用 |
|---|---|
| `probe [--json]` | 报告设备 / KMI / ksud / cgroup 事实 |
| `cap [--json]` | 报告 core 的能力与状态 |
| `status [--json]` | 已加载模块与 `/proc/dnsk/status` |
| `monitor [--json]` | 每次调用输出一份机器可读状态快照 (供轮询) |

## 加载与卸载

| 命令 | 作用 |
|---|---|
| `preflight [feature]` | 跑设备侧预检, 默认 `core` |
| `load <feature>` | 加载一个 feature(幂等, 重复调用不报错) |
| `unload <feature>` | 安全卸载: 查引用, 查依赖, 卸载后复验 |
| `start-all [--json]` | 按依赖顺序加载全部随包模块 |
| `stop-all [--json]` | 先卸 feature 再卸 core, 每步复验 |
| `profile <name>` | 加载一个命名 profile |

`load` 的符号绑定后端由 `DNSK_LOADER` 选择:

- `ksud`(默认)-- 走 `/proc/kallsyms` 加 KernelSU 驱动
- `lkmins` -- 只走 `/proc/kallsyms` 再调 `init_module(2)`

## 开机行为

| 命令 | 作用 |
|---|---|
| `enable` / `disable` | 允许 / 跳过下次开机的 core 自动加载 |
| `boot-begin` / `boot-ok` / `boot-check` | 开机守卫的三步, 供 init 脚本调用 |

## 诊断

| 命令 | 作用 |
|---|---|
| `logs [lines]` | 查看模块状态日志尾部 |
| `doctor [--last-panic]` | 收集 dmesg / pstore / panic 证据 |

`doctor --last-panic` 是排查设备崩溃的入口: 它把 pstore 与 dmesg 的相关片段
一起取出来, 不需要手工翻 `/sys/fs/pstore`

## cgroup 准备

| 命令 | 作用 |
|---|---|
| `cgroup-prep [--plan]` | 探测 cgroup v1/v2 路径, **不做修改** |
| `cgroup-prep --bind <root>` | 把已有 `/dev/{memcg, cpuctl, cpuset, blkio}` 按同名绑进 `<root>` |
| `cgroup-prep --delegate <dir>` | 在 `<dir>` 的 `cgroup.subtree_control` 开启 cpuset/cpu/io/memory/pids(仅 v2) |

`--plan` 只打印将要做什么, 用于确认; 不加 `--plan` 才真正执行

## 存储

| 命令 | 作用 |
|---|---|
| `storage-loop create <img> <size> [ext4\|f2fs]` | 创建稀疏 loop 镜像, 不挂载 |
| `storage-loop remove <img>` | 卸载 (尽力) 并删除镜像 |

## 帮助

`dnskctl help` 打印上述命令清单
命令的**实现**是权威, 本文件只描述契约;
两者不一致时以脚本为准并修正本文件
