# KernelPatch/KPM 与 LKM 评估（研究记录，未实施）

日期：2026-11-06。目标设备 beryl / Android 16 /
`6.12.30-android16-5`，arm64，4 KiB 页，KMI `android16-6.12`，
KernelSU ReSukiSU 4.2.0-rc3 已 late-load。本文只用于决策。

## 结论

KPM 只适合做热路径 guest，不能替代 LKM。已有 KSU 时，唯一路径是
APatch jailbreak late-load 加载 `kernelpatch.ko`，这是第二套 root 栈；
6.12 LKM 路径有首次 hook Oops 的公开记录。v1.0.0 保持 LKM-only，
KPM 仅作为 M6 的云端/QEMU 预研，不在日常设备上双栈混用。

## 加载路径（设备实测 + 源码）

- `apd 11282` 位于 `/data/adb/ap/bin/apd`（不是 `/data/adb/ap/apd`）。
- `apd late-load` 只加载 KernelPatch 运行时：
  `--module <ko> --kmi <KMI> [--magica <port>] [--package-name <pkg>]`；
  读取 `/data/adb/ap/{kmi}_kernelpatch.ko`，手工用 `/proc/kallsyms`
  重定位后 `init_module(2)`，应用 Magisk SELinux policy 并恢复 enforce=1。
- **apd 没有 KPM 子命令**；KPM 的 load/control 由 APatch 管理器 JNI 以
  管理器 uid 发 SuperCall，LKM 按 uid 认证，不使用 superkey。
- 前置：KSU root；`CONFIG_MODULES` 且未 `MODULE_SIG_FORCE`；
  `KALLSYMS_ALL`；SELinux 临时 permissive；安装并初始化 APatch 管理器；
  ko 匹配 KMI；pstore/回滚。KSU 与 KP 双栈无官方共存支持证据。
- 另一条路是给 KSU LKM 编 `CONFIG_KPM=y`（SukiSU 系），需重编 KSU；
  当前 ReSukiSU 4.2.0-rc3 未见启用的公开证据。

## 能力与边界

可做：inline hook、syscall hook、函数指针替换、指令 hotpatch、
`kallsyms_lookup_name`、kstorage/task_ext/BTF offset。KPM 是 bare-metal，
生成代码无 kCFI/PAC/SCS；KP 对 CFI/PAC/SCS 有 target 相关绕过，不是全局关闭。

明确做不到/不可靠：

- 新增真实 `cgroup_subsys`（`cgroup_subsys[]`、`enum cgroup_subsys_id`、
  `css_set->subsys[]` 均编译期固定）；
- 扩 syscall 表或固定数组；改变编译期 config/结构体布局；
- 提供 `struct module` 的 owner/refcount/signature/CRC；
- 卸载时 in-flight 回调无保护（`kp_unload_module` 直接 list_del→exit→free）。

## 性能

截至 2026-11-06 没有公开的 KPM vs LKM 基准。按机制预期：

| 方式 | 命中开销 |
|---|---|
| KPM inline hook | 低（入口分支 + trampoline） |
| LKM kprobe | 较高（BRK 异常/通知链） |
| LKM ftrace | 低（patchable NOP → direct call） |
| LKM syscall 表 / 文本 inline patch | 低 |

测法草案（未执行）：同一 `getpid` 循环 1e7 次、绑核定频；A 基线、
B KPM 空 before、C LKM kprobe、D LKM ftrace、E LKM syscall 表、
F LKM 文本 inline patch；`simpleperf stat -r 10 -e cycles,instructions`
取 cycles/op，`clock_gettime` 取中位数/p99。注意 `hook_syscalln` 的
global 版本会让所有 syscall 走共享 trampoline，测“命中开销”要用
`fp_hook_syscalln`/`inline_hook_syscalln`。

## 稳定性

- 版本绑定强：KPM 与 KP 头文件/ABI 绑定且无版本检查，KMI 专用 ko，
  无 CRC；ABI 漂移静默。
- `sc_kpm_unload` 存在但无 module ref，KPM 必须自己 unhook；曾有
  RCU 自死锁/双重释放修复（#317/#296）。
- panic 只能重启；本项目 Bootloader 锁定、冷重启丢 KSU。
- 近证：KP #307（6.12 LKM 首次 hook Oops，已关闭但修复版本未核实）、
  #319（6.6 .ko panic，open）、#300（6.12 镜像 boot 失败，open）；
  APatch 11282 发布说明仍称 KPM 适配性有问题、升级需先卸载已嵌入 KPM。
- 上游活跃但年轻：0.13.9 于 2026-09-24 发布；LKM 2026-08 才加入，
  README 仍自称 skeleton。

## 功能覆盖

| 功能 | KPM 结论 |
|---|---|
| PID/IPC/SysV/mqueue/userns | 可做，工作量大；全 hook+数据 patch，逐 KMI 适配 |
| tmpfs xattr/ACL | 可做；卸载时 inode 仍引用 KPM ops 即 UAF |
| xt/ipset/REJECT/RECENT | 部分可做；`xt_target.me=NULL` 无 owner/ref |
| legacy ip6table_nat | 可做但脆；依赖 CFI 放行 |
| cgpids 虚拟控制器 | 行为可近似；真实 cgroup_subsys 不可能 |
| 需 `struct module` 的注册 | 不可靠；生命周期自负 |

## 本机实验步骤（只列，未执行）

1. 兼容性：`uname -r`、KMI、页大小、`/proc/config.gz` 的
   MODULES/KALLSYMS_ALL/MODULE_SIG_FORCE/CFI/SCS/PAC/BTI/LTO；
   下载 0.13.9 `android16-6.12_kernelpatch.ko` 并校验；确认 APatch
   管理器/apd/ko 版本。
2. 回滚准备：pstore/ramoops、`panic_on_oops`、冷重启后 KSU 恢复流程、
   `selpermissive.ko`、原始 dmesg；不写 boot 分区。
3. late-load：`su -c '/data/adb/ap/bin/apd late-load --module ... --kmi
   android16-6.12 --package-name me.bmax.apatch'`；确认 dmesg
   `KernelPatch LKM ready`。
4. 最小 KPM：`demo-hello.kpm` 经 APatch 管理器加载，确认 list/info/unload。
5. 最小 hook + 基准：`demo-inlinehook` 选冷函数，空 before/after，
   exit 必须 unhook；按上表 A–F 测；看 dmesg/pstore。
6. 卸载/恢复：管理器 unload 全部 KPM → `rmmod kernelpatch`（或重启）；
   确认无 oops、hook 已清、KSU 可用。

每步的风险：ko 与厂商 6.12.30 编译常量不一致（#307 的 FIXADDR_TOP）、
SELinux/签名/vermagic/双栈冲突、一次 Oops 即重启并丢 KSU、未 unhook
即卸载导致 UAF。

## 决策标准

值得引入 KPM 需同时满足：功能只能用运行期 inline hook 表达、LKM
kprobe/表 patch 做不到；目标是冷/低频函数；可接受 panic 重启；有
pstore + 人工回滚；不要求开机自启；锁定单一 KMI；实测命中开销显著优于
LKM。当前 beryl 的 PID/IPC/tmpfs/netfilter/cgpids 都能用 LKM 覆盖，
KPM 没有功能增量；v1.0.0 不引入。

## 来源（访问 2026-11-06）

- KernelPatch KPM 文档：<https://github.com/bmax121/KernelPatch/blob/main/doc/en/module.md>
- syscall hook：<https://github.com/bmax121/KernelPatch/blob/main/doc/en/syscall-hook.md>
- APatch apd late-load/insmod/CLI：<https://github.com/bmax121/APatch/tree/main/apd/src>
- KP issues #307/#319/#300：<https://github.com/bmax121/KernelPatch/issues/307>、
  <https://github.com/bmax121/KernelPatch/issues/319>、
  <https://github.com/bmax121/KernelPatch/issues/300>
- KP LKM 源码：<https://github.com/bmax121/KernelPatch/tree/main/lkm>
- KP 0.13.9：<https://github.com/bmax121/KernelPatch/releases/tag/0.13.9>
- APatch 11282：<https://github.com/bmax121/APatch/releases/tag/11282>
- SukiSU KPM 配置：<https://github.com/SukiSU-Ultra/SukiSU-Ultra/blob/main/kernel/Kconfig>
