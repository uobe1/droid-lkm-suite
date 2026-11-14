# QEMU 指南

当前仓库尚未提交 QEMU 结果; `tests/qemu/run-qemu.sh` 是真实 runner, 缺少
`KERNEL`/`INITRD`/`MODULE` 时会失败, 不会伪造通过

## 准备

1. 从 AOSP 同 KMI 源码构建 feature-off 内核 `Image`
2. 构建带 `/init` 的 arm64 initramfs; `/init` 通过 9p 挂载 hostshare, 执行
 `insmod /mnt/dnsk_core.ko`, 读取 `/proc/dnsk/status`, 成功打印
 `DNSK_QEMU_OK`
3. 用同一 KDIR 构建 `dnsk_core.ko`(以及后续 feature 模块).

## 运行

```sh
KERNEL=out/Image INITRD=out/initramfs.cpio.gz \
MODULE=build/kmi/android16-6.12/dnsk_core.ko \
tests/qemu/run-qemu.sh
```

多 KMI 矩阵, Docker/NixOS/UFW 和差分测试在 CI 分片中执行; 任何
`panic/Oops/BUG/CFI failure/refcount_t/Unknown symbol` 都视为失败
