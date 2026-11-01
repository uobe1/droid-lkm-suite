# ABI build profiles

## 问题

KMI 只冻结导出符号和 KMI 类型，不冻结 config 相关的内部 struct 布局。
设备 6.12.30 MTK 的 `CONFIG_NETFILTER_XTABLES_COMPAT=n` 使
`struct xt_match/xt_target` 为 104 字节；DDK/GKI 6.12 默认是 128 字节。
用 128 构建在 104 设备上加载会在 `xt_register_matches` 崩溃.

## 机制

- profile schema 2: `config` 必须包含 registry 的全部 knob (xtables_compat、
  锁/RT/KASAN、netfilter/conntrack、cgroup、fs、page size/VA bits 等);
  缺失/未知 key 直接拒绝, 不回退默认值. 见
  [abi-config-scope.md](abi-config-scope.md).
- 只有可宏化的 knob 输出构建变量: `DNSK_ABI_PROFILE`、`DNSK_XT_NOCOMPAT`/
  `DNSK_XT_LAYOUT`、`DNSK_PAGE_SIZE`、`DNSK_VA_BITS`、`DNSK_MAX_SKB_FRAGS`、
  `DNSK_CGROUP_SUBSYS_COUNT`; 其余 knob 由选择匹配 config 的 DDK 构建保证,
  不能用 shim 伪装.
- `profiles/abi/android16-6.12-xt104.json` (设备默认): `xtables_compat=0`,
  `DNSK_XT_NOCOMPAT=1`, `DNSK_XT_LAYOUT=104`.
- `profiles/abi/android16-6.12-xt128.json` (stock GKI/DDK 6.12):
  `xtables_compat=1`, `DNSK_XT_LAYOUT=128`; `validated:false`.
- `tools/build_flags.py` 校验 schema/config/KMI, 输出 shell 变量; CI 只在
  `android16-6.12` 矩阵条目应用 profile, 未建模的 KMI 不使用 6.12 profile.
  构建后校验 `.ko` 内的 `dnsk_xt_layout=` 与 `dnsk_abi_profile=` stamp.
- CI `workflow_dispatch` 输入 `build_profile`, 默认 xt104; `push` 也使用
  默认值. `package-dev` 用同一 profile 把 `xt_layout` 写入 `policy.conf`.
- `features/ipset` 与 `features/netfilter` 的 no-compat shim 在首次包含
  `x_tables.h` 前关闭 `CONFIG_NETFILTER_XTABLES_COMPAT`; shim 保持在
  `LINUXINCLUDE` 最前. `core/include/dnsk_xt_layout.h` 对 104/128 两种布局
  静态断言 size 和全部字段偏移, 并生成 `dnsk_xt_layout=` 模块 stamp.
- `dnskctl load ipset|netfilter` 先比对 `.ko` 的 `dnsk_abi_profile`/
  `dnsk_xt_layout` stamp 与 `policy.conf`, 不一致即拒绝; 再跑
  `dnsk-preflight` 从设备 BTF 校验 size/全部成员偏移 (104/128). 任一失败即
  拒绝 `ksud insmod`; `policy.conf` 缺少 profile 字段同样拒绝.

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
