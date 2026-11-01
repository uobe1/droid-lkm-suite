# ABI build profiles

## 问题

KMI 只冻结导出符号和 KMI 类型，不冻结 config 相关的内部 struct 布局。
设备 6.12.30 MTK 的 `CONFIG_NETFILTER_XTABLES_COMPAT=n` 使
`struct xt_match/xt_target` 为 104 字节；DDK/GKI 6.12 默认是 128 字节。
用 128 构建在 104 设备上加载会在 `xt_register_matches` 崩溃.

## 机制

- `profiles/abi/android16-6.12-xt104.json` (设备默认):
  `"abi":{"xtables_compat":0}` -> `DNSK_XT_NOCOMPAT=1`,
  `DNSK_XT_LAYOUT=104`.
- `profiles/abi/android16-6.12-xt128.json` (stock GKI 6.12):
  `"xtables_compat":1` -> compat 构建, `DNSK_XT_LAYOUT=128`.
- `tools/build_flags.py` 校验 schema/abi key, 输出 shell 变量; CI 构建步骤
  `eval` 这些变量并传给 ipset/netfilter Kbuild.
- CI `workflow_dispatch` 输入 `build_profile`, 默认 xt104; `push` 也使用
  默认值. `package-dev` 用同一 profile 把 `xt_layout` 写入 `policy.conf`.
- `features/ipset` 与 `features/netfilter` 的 no-compat shim 在首次包含
  `x_tables.h` 前关闭 `CONFIG_NETFILTER_XTABLES_COMPAT`;
  `core/include/dnsk_xt_layout.h` 静态断言 104 字节及全部字段偏移.
- `dnsk-preflight` 读取设备 BTF, 校验 size/全部成员偏移 (104/128);
  `dnskctl load` 先跑 preflight, 失败即拒绝 `ksud insmod`.

## 验证

- 设备静态 preflight: `xt_match_size=104`, `xt_target_size=104`,
  `xt_ok=true`, `ok:true`.
- 设备 ipset/netfilter active load/unload 通过 (见各自 evidence).
- 主机测试覆盖 104/128 合成 BTF 正例与 128-vs-104 反例.

## 边界

- 目前只建模 `xtables_compat`; 其他 GKI 未冻结且影响边界 struct 的 config
  (RT/LOCKDEP/KASAN/RANDSTRUCT/NETFILTER 子配置等) 正在由子代理枚举,
  后续按同一 profile/preflight 机制扩展.
- `xt128` 仅用于 stock GKI/QEMU 或对应设备; 设备包必须选择与设备 BTF
  一致的 profile, 不能只按 `uname` KMI.
