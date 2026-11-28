# droid-lkm-suite

Native/Real LKM container-subsystem completion for Android GKI

本页是 MkDocs 首页
当前处于 `android16-6.12` 开发收尾阶段 (core M0 完成, feature 处于开发/Native 化), 功能状态见 [status](status.md).

## 关键约束

- Bootloader 锁定: 绝不写 boot/vendor_boot/dtbo/system/vendor_dlkm/system_dlkm
- 只用 `dnskctl load`/`unload`; 不使用 `/system/bin/insmod` 或 busybox insmod
- Native 或 Real, 无 STUB/FAKE_SUCCESS
- 当前只针对 KMI `android16-6.12`; 其余 7 个 KMI 已搁置, 不建 tag

## 导航

- 架构
  - [概览](architecture/overview.md)
  - [模块生命周期](architecture/module-lifecycle.md)
  - [Hook 机制](architecture/hook-mechanism.md)
  - [卸载安全](architecture/unload-safety.md)
  - [ABI 与 profile](architecture/abi-and-profiles.md)
- [状态矩阵](status.md)
- API
  - [Kernel API](api/kernel.md)
  - [Userspace API](api/userspace.md)
- [构建](guides/build.md)
- [dnskctl 命令手册](guides/dnskctl.md)
- [新增一个 feature](guides/new-feature.md)
- [真机测试](guides/device.md)
- [QEMU](guides/qemu.md)
- [测试](guides/testing.md)
- [文档规范](STYLE.md)
