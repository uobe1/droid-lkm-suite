# 设备 xt_register_matches panic: 根因与修复

## 结论

这是 **dnsk_ipset 的 ABI 布局不兼容**, 不是外部旧模块.

最新的 09:07:59 panic 的内核日志给出了完整调用栈:

```text
CPU: 7 UID: 0 PID: 28702 Comm: ksud
Call trace:
  xt_register_matches+0x50/0xb8
  dnsk_ipset_xt_set_init+0x20/0x68 [dnsk_ipset]
  init_module+0x2c/0xf90 [dnsk_ipset]
  do_init_module
  load_module
  __arm64_sys_init_module
```

`x19` 是 match 数组指针, `x20=7`, fault address `0x8`;
`xt_register_matches` 在遍历 match 数组时读到了错位的 family/list.

## 根因

| 对象 | `struct xt_match`/`xt_target` |
|---|---|
| 设备 6.12.30 MTK BTF | 104 字节, 无 `compat_from_user/compat_to_user` |
| DDK 6.12.76 构建的 dnsk_ipset | 128 字节, 含 compat 字段 |

模块内的静态 `set_matches[]`/`set_targets[]` 按 128 字节布局编译, 设备内核
按 104 字节步长遍历; 第 8 个元素开始错位, family 被读成 7, 随后 list 指针
为 NULL, 在 `xt_register_matches` 内崩溃. QEMU 使用 DDK 6.12.76 内核,
布局与模块一致, 因此 QEMU 一直通过, 掩盖了设备差异.

04:49 的 panic 与 09:07 是同一签名 (`xt_register_matches+0x50`), 也是
active 测试加载 dnsk_ipset 导致; 04:00 的 `nft_unregister_expr` panic 是
另一个已修复的 netfilter inactive 卸载 bug.

## 修复

- 设备构建使用 `DNSK_XT_NOCOMPAT=1`:
  - `features/ipset/compat_off.h` 强制 `#undef CONFIG_COMPAT`;
  - `features/ipset/Kbuild` 通过 `-include` 在 `x_tables.h` 之前生效;
  - `features/ipset/main.c` 加入编译期
    `_Static_assert(sizeof(struct xt_match) == 104)` 与
    `xt_target == 104`.
- QEMU/stock GKI 仍使用库的默认 compat 布局 (128 字节), 继续跑 QEMU 语义测试.
- `features/ipset/Makefile` 显式把 `DNSK_XT_NOCOMPAT` 传给 kernel make;
  CI 的 android16-6.12 设备产物构建 no-compat 版本.

## 验证状态

- No-compat 模块正在由 CI 构建; 必须确认编译期断言通过, 且产物 DWARF/BTF
  与设备 104 字节布局一致.
- 验证完成前不得再次在设备加载 ipset; 下一次设备加载只做 insmod/rmmod,
  先确认布局匹配.
- 后续需要给 `dnsk_preflight` 增加 BTF `xt_match/xt_target` 布局检查,
  防止其他 KMI/厂商内核再次出现同类 ABI 差异.
