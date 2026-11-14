# droid-lkm-suite

Native/Real LKM container-subsystem completion for Android GKI.

本页是 MkDocs 首页。当前处于 M1 开发阶段（android16-6.12），功能状态见 [status](status.md)。

## 关键约束

- Bootloader 锁定：绝不写 boot/vendor_boot/dtbo/system/vendor_dlkm/system_dlkm。
- 只用 `ksud insmod`；不使用 `/system/bin/insmod` 或 busybox insmod。
- Native 或 Real，无 STUB/FAKE_SUCCESS。
- 首 tag 需要全部 8 个 KMI 与全部目标功能通过；未完成不打 tag。

## 导航

- [架构](architecture/overview.md)
- [状态矩阵](status.md)
- [构建](guides/build.md)
- [dnskctl 命令手册](guides/dnskctl.md)
- [新增一个 feature](guides/new-feature.md)
- [真机测试](guides/device.md)
- [文档规范](STYLE.md)
