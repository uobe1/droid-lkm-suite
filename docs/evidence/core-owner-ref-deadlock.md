# core/feature 引用死锁与无重启恢复

## 现象

`dnsk_ns` 注册状态桥后, `dnsk_ns` 与 `dnsk_core` 互相持有模块引用:

```text
dnsk_ns               319488  1
dnsk_core              45056  1
```

`rmmod dnsk_ns` 返回 `Try again`, 因为 core 的 feature 注册表对 owner 调用了 `try_module_get`;
同时 ns 通过 `dnsk_core_get()` 持有 core 引用。两者都无法进入各自的 `module_exit`, 形成死锁。
设备启用了 `CONFIG_MODULE_UNLOAD` 但未启用 `CONFIG_MODULE_FORCE_UNLOAD`。

## 根因

注册表用模块引用保证“feature 未注销就不能卸载”, 但 feature 的正常注销路径在
`module_exit` 里; 引用本身阻止了 `module_exit` 执行。hook 注册表对 owner 的引用存在同一问题。

## 修复

- feature/hook 注册表不再对 owner 取引用, owner 字段仅用于诊断。
- 每个 feature 必须在 `module_exit` 中删除自己的注册和 hook。
- `dnsk_core_get()` 仍然对 core 取一个引用, 调用方在最后一次使用后 `module_put(owner)`;
  因此 feature 存活期间 core 不能被卸载, 但 feature 可以正常退出并释放该引用。

## 无重启恢复

临时构建 `dnsk_unstick.ko`（未进入发布产物）: 调用 `dnsk_core_get()` 后执行
`feature_del("ns")`, 释放旧 core 对 ns 的引用。随后按 `dnsk_unstick -> dnsk_ns -> dnsk_core`
顺序卸载:

```text
dnsk_unstick: feature_del(ns) = 0
unstick_rc=0
ns_rc=0
core_rc=0
all_gone
proc_gone
```

恢复过程没有重启设备, 没有 Oops。设备 `boot_id` 在恢复前后均为
`c8716b4b-091c-4bc1-87e8-925d9c095154`。

## 修正版验证

加载修正后的 `dnsk_core.ko` 与 `dnsk_ns.ko`（`gki=1 activate=0`）:

```text
bridge_status=ok
core_pinned_by_feature=ok
CORE_BRIDGE_SMOKE_OK
```

`core_pinned_by_feature=ok` 表示 ns 持有 API 引用时 `rmmod dnsk_core` 被拒绝;
ns 卸载后 core 可以正常卸载, `/proc/dnsk/status` 消失。dmesg 无 Oops/BUG。
