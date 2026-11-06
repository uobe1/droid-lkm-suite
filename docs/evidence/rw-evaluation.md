# 任意内存读写（RW）路线评估（研究记录，未实施）

日期：2026-11-06。目标内核：beryl / Android 16 /
`6.12.30-android16-5`，arm64，4 KiB 页，GKI `android16-6.12`。
本文只用于决策，不代表任何 RW 能力已实现或通过验收。

## 1. 有 root（KernelSU）时的路径

- GKI `android16-6.12` gki_defconfig 未启用 `CONFIG_DEVMEM`；`/dev/mem`、
  `/dev/kmem` 不可用（后者自 5.13 删除），`STRICT_DEVMEM` /
  `iomem=relaxed` 无意义。
- `/proc/kcore`：CAP_SYS_RAWIO 的只读 ELF 视图；只有虚拟地址读，无写回调。
- `/proc/iomem` 只有地址表；kprobe/tracefs 可按符号注册但只读；
  BPF 只有 `bpf_probe_read_kernel`，无任意写。
- 结论：任意（物理）读写只能来自 LKM/KPM 内部：RAM 用线性映射，
  MMIO 用 ioremap，非映射 VA 走页表遍历；“任意物理”实际只覆盖 RAM。
- 限制：`kptr_restrict=2`、`perf_event_paranoid=3` 使 root 也拿不到符号
  地址；`panic_on_oops=1`，一次 oops 即重启并丢失 KernelSU root。

## 2. 无 root 路径（截至 2026-11）

- **CVE-2026-43499（GhostLock）**：Linux CNA 记录 2026-05-21，CWE-416，
  CVSS 7.8，修复 commit `3bfdc63936dd`（2026-04-21），Fixes 2011 年
  `8161239a8bcc`；6.12.x < 6.12.86 受影响。本机 6.12.30 在区间内，
  但厂商是否回移**未核实**。公开链：Nebula Security IonStack
  （kernelCTF），Pixel 10/Android 17 6.12 有公开利用；arm64 CFI/KASLR
  绕过已公开但强 target 相关。触发为 `FUTEX_CMP_REQUEUE_PI` 三线程 PI
  死锁，普通 app 可发。“97%/5 秒”仅二手报道，未核实。
- **OpenAI“约 900 个漏洞”**：官方 2026-11-04 博文
  《A thousand fewer paths to root》称 2308 候选、1724 复现、948 个去重
  LPE；细节与 CVE 列表未公开，**不能用于本内核**。
- 其他：epoll 嵌套 UAF `CVE-2026-46242`（6.12.x < 6.12.95，覆盖本机，
  未见公开利用）；Dirty Pagetable/PTE 重写、cross-cache（SLUBStick）
  仍通用；pipe_buffer 已修；nftables UAF 因 `NF_TABLES` 未编入不可达；
  io_uring/userfaultfd 的 seccomp/sysctl 门槛未核实。

## 3. UAF 到物理读写

页级 UAF → cross-cache 归还 buddy → 回收为用户 PTE 页（4K 页，512 项；
VA_BITS_39、3 级表）→ 改 PTE 指向目标物理页 → TLB 刷新 → 任意物理读写。
KASLR：`kptr_restrict=2` 断 kallsyms，KPTI 下 prefetch 不稳，需要
physmap/侧信道。CFI：函数指针只能换同型函数；PAC 只签返回地址，
数据指针未签，数据面攻击不受阻。

## 4. 原子性与回滚

- 数据：对齐 8B 单拷贝原子（READ_ONCE/WRITE_ONCE）；指针用
  rcu_assign_pointer + synchronize_rcu；多字段用 seqlock 或整块双缓冲
  + 指针交换；static_key_slow_inc/dec 幂等可回滚。
- 文本：只改 4B 对齐单指令；跨 CPU 用 `aarch64_insn_patch_text()`
  （stop_machine + I-cache）；`_nosync` 仅限未执行代码。两个函数未导出，
  需 kprobe 解析。`set_memory_rw` 全局生效、易检测，不适合常驻。
- 可回滚：保留原字节/原指针，stop_machine 回写并等 RCU 宽限。
  不可回滚：结构体布局/大小、分配器状态、未记录 PTE、编译期配置。
  多指令改序只能停机式。

## 5. 可行性矩阵

| 方法 | root | 稳定性 | 可检测性 | 工作量 | 回滚 |
|---|---|---|---|---|---|
| LKM 数据/指针（RCU/static_key） | 是 | 高 | 低-中 | 低 | 好 |
| LKM 文本单指令 patch | 是 | 中 | 中 | 中 | 好 |
| LKM 任意 PA（线性映射/memremap） | 是 | 高（仅 RAM） | 低 | 中 | 好 |
| /proc/kcore、kprobe、BPF | 是 | — | — | — | 只读 |
| /dev/mem、/dev/kmem | 不可用 | — | — | — | — |
| GhostLock→PTE（无 root） | 否 | 低（panic 即丢 root） | 中 | 极高 | 无 |
| Dirty Pagetable/cross-cache | 否 | 中 | 中 | 高 | 无 |
| nftables UAF | 不可达 | — | — | — | — |
| io_uring/uffd | 否 | 未核实 | 高 | 高 | 无 |

优先顺序：LKM 数据/static_key → 文本单指令 → 任意 PA。
直接放弃：`/dev/mem`、nftables、以无 root 链作交付。

## 6. 条件式结论

只有同时满足才值得评估 RW：目标语义是运行期数据（ops 表、函数指针、
静态数组）；单写者、幂等、可逆；LKM hook 无法表达（内联或无表项）。
门禁：QEMU + 真机长时压测零 panic、字节级回滚与启动自检、厂商补丁状态
核实。编译期配置（NF_TABLES、页大小、KMI/结构体布局）不能被 RW 弥补；
禁止以“内存已改”当功能通过。无 root 链只作研究，不作依赖。

## 来源

- CVE-2026-43499：<https://cveawg.mitre.org/api/cve/CVE-2026-43499>；
  修复：<https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=3bfdc63936dd4773109b7b8c280c0f3b5ae7d349>
- CVE-2026-53166（REJECTED）：<https://cveawg.mitre.org/api/cve/CVE-2026-53166>
- CVE-2026-46242：<https://cveawg.mitre.org/api/cve/CVE-2026-46242>
- OpenAI：<https://openai.com/index/a-thousand-fewer-paths-to-root>
- GKI defconfig：<https://raw.githubusercontent.com/aosp-mirror/kernel_common/android16-6.12/arch/arm64/configs/gki_defconfig>
- Dirty Pagetable：<https://yanglingxi1993.github.io/dirty_pagetable/dirty_pagetable.html>
