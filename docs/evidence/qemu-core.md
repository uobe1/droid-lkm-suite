# QEMU core smoke (stock GKI)

## 环境

- KMI: `android16-6.12`, DDK 容器
  `ghcr.io/ylarod/ddk-min:android16-6.12-20260828`
- 内核: DDK kdir 自带的 Linux `6.12.76-4k` `vmlinux`
- 启动: `llvm-objcopy` 生成 arm64 `Image`, Alpine 3.20.3 arm64 minirootfs
  作为 initramfs, 模块内嵌, QEMU TCG `-M virt -cpu max`
- 工作流: `.github/workflows/qemu-core.yml` (mode `baseline`)

## 首次失败与修复

首次启动 (run 41239353915) 时 core 因 GKI 未导出
`__set_fixmap` 和 `copy_to_kernel_nofault` 无法加载:

```text
dnsk_core: Unknown symbol __set_fixmap (err -2)
dnsk_core: Unknown symbol copy_to_kernel_nofault (err -2)
insmod: can't insert '/modules/dnsk_core.ko': unknown symbol in module
```

两者已改为在 `dnsk_patch_init()` 中经 core 的 kallsyms resolver 解析,
并通过 `__nocfi` 包装调用; 缺少任一符号时 patch 直接 disabled, 不影响加载.

## 通过结果

run `41240135015` (commit `d0853f9`) 的 `boot` job:

```text
DNSK_QEMU_BOOT 6.12.76-4k
DNSK_QEMU_CORE_READY
DNSK_QEMU_OK
qemu_rc=0
```

init 验证了 `/proc/dnsk/status` 的 `patch=ready` 与 `panic=ready`,
然后卸载 core 并确认 `/proc/dnsk/status` 消失; 日志无 panic/Oops/BUG/CFI failure.

## 边界

`ns`/`devtmpfs`/`netfilter` 等 Droidspaces 深移植会引用 GKI 未导出的内核内部
符号, 由目标设备的 KernelSU `ksud` 加载器解析; stock QEMU 的普通 `insmod`
不能加载它们. 因此该工作流的 baseline 只验证 core 与运行时符号路径.
feature 激活在 KSU 真机上验证, 或将来的 KSU-enabled VM.
`dnsk_ipset` 是例外: 它的导入全部通过导出检查, 可在 stock QEMU 上直接测试.
