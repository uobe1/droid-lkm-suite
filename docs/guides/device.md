# 真机测试

## 原则

- 只通过设备上的 `ksud insmod` 加载；不使用 `/system/bin/insmod`。
- 不在本文件记录具体主机地址、密钥、容器路径等个人环境信息；这类内容属于
  本地的 Note/AGENTS/Memory 层（见 CONTRIBUTING）。
- 高风险模块默认手动加载；boot guard 失败进入 safe mode。

## 加载与状态

在设备 root shell（`su -c` / KSU su）中：

```sh
ksud insmod /data/adb/modules/droid_lkm_suite/kmi/<kmi>/dnsk_core.ko
/data/adb/modules/droid_lkm_suite/bin/dnskctl preflight core
/data/adb/modules/droid_lkm_suite/bin/dnskctl status --json
/data/adb/modules/droid_lkm_suite/bin/dnskctl load <feature>
```

`ns` / `devtmpfs` / `netfilter` 由 `dnskctl` 自动附加 `gki=1 activate=1`；
手工 insmod 时需自行传入，否则模块会按设计拒绝加载。

## 边界

- 不做 boot/vendor_boot/dtbo/system/vendor_dlkm/system_dlkm 写入。
- 真机允许崩溃测试；panic 后从 pstore 取证，`dnskctl doctor --last-panic` 解析。
- 重负载（Docker/NixOS/UFW/Fail2ban）在云端 QEMU 执行；本机只做最终加载/smoke。
- SELinux 保持 Enforcing；禁止 permissive。
