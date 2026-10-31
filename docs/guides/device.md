# 真机测试

## 入口

SSH 主机：`termux.native`。从 droid-distro chroot 执行时，若 OpenSSH 因 `/root/.ssh/config` 所有权拒绝，使用显式参数：

```sh
ssh -F /dev/null -i /root/.ssh/UPBR_Build0 -o IdentitiesOnly=yes \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -p 8022 u0_a264@127.0.0.1 'su -c "uname -r"'
```

## 加载

`ksud` 主路径是 `su -c`；`sudo` 仅用于 Termux 侧工具，当前 `sudo ksud` 不可用：

```sh
ssh termux.native 'su -c "ksud insmod /data/adb/modules/droid_lkm_suite/kmi/android16-6.12/dnsk_core.ko"'
ssh termux.native 'su -c "/data/adb/modules/droid_lkm_suite/bin/dnskctl preflight core"'
ssh termux.native 'su -c "/data/adb/modules/droid_lkm_suite/bin/dnskctl status --json"'
```

## 边界

- 不做 boot/vendor_boot/dtbo/system/vendor_dlkm/system_dlkm 写入。
- 真机允许崩溃测试；panic 后从 pstore 取证，`dnskctl doctor --last-panic` 解析。
- 高风险模块默认手动加载；boot guard 失败进入 safe mode。
- 重负载（Docker/NixOS/UFW/Fail2ban）在云端 QEMU 执行；本机只做最终加载/smoke/已有 droidspaces 容器。
- SELinux 保持 Enforcing；禁止 permissive。
