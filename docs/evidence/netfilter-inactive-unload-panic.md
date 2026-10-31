# netfilter inactive unload panic — root cause and fix

日期：2026-10-31。设备 `6.12.30-android16-6.12`，内核 Oops 后由 panic 重启。

## pstore 证据

`/sys/fs/pstore/console-ramoops-0`：

```text
droidspaces_netfilter: inactive: activate=1 and gki=1 are required
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Internal error: Oops: 0000000096000006 [#1] PREEMPT SMP
pc : nft_unregister_expr+0x24/0x78 [dnsk_netfilter]
lr : nft_unregister_expr+0x1c/0x78 [dnsk_netfilter]
 ds_nf_exit_nft_reject_ipv6+0x18/0x28 [dnsk_netfilter]
 cleanup_module+0x3c/0x688 [dnsk_netfilter]
```

## 根因

`dnsk_netfilter` 以 `activate=0` 加载时 init 直接返回 0，没有注册任何 nftables
unit；但 `module_exit` 无条件遍历 `units[]` 调用 `exit()`。`nft_unregister_expr()`
对未初始化的表达式做 NULL 解引用，内核 panic。

这不是 `su -c reboot`，也不是 ns/devtmpfs 触发。

## 修复

`vendor/netfilter/main.c` 增加 `registered` 标志：

- 只有全部 unit 注册成功后才置 true；
- `module_exit` 在 `registered == false` 时直接返回，不触碰未初始化的 unit；
- 部分初始化失败的原有逆序回滚保留。

## 验证门控

修复构建完成后，在干净设备上：

1. `ksud insmod dnsk_netfilter.ko gki=1 activate=0`
2. `busybox rmmod dnsk_netfilter`
3. 检查 `/sys/fs/pstore` 无新 Oops、设备不重启。

## 回归结果（2026-10-31）

`boot_id` 前后一致：`c8716b4b-091c-4bc1-87e8-925d9c095154`。

```text
Loaded kernel module: .../dnsk_netfilter.ko
nf_ins=0
nf_rm=0
BOOT_ID_SAME
no_dnsk_modules
droidspaces_netfilter: inactive: activate=1 and gki=1 are required
```

无新 Oops/panic。inactive 卸载崩溃已修复。active（`activate=1`）注册尚未测试，
netfilter 仍为 development/unsupported。
