# BTF 布局 preflight 与 xtables ABI 修复

## 背景

设备 6.12.30 MTK 的 `struct xt_match`/`xt_target` 是 104 字节
(`CONFIG_NETFILTER_XTABLES_COMPAT=n`), DDK 6.12.76 默认构建是 128 字节.
内核按 104 步长遍历 128 数组会在 `xt_register_matches` 崩溃; KernelSU
加载器只解析符号, 不修正结构布局.

## 修复

- `core/include/compat_include/linux/netfilter/x_tables.h`: 对
  `DNSK_XT_NOCOMPAT=1` 构建, 在首次包含 `x_tables.h` 前 undef
  `CONFIG_NETFILTER_XTABLES_COMPAT`.
- `core/include/dnsk_xt_layout.h`: 静态断言 104 字节及
  `list/name/revision/match|target/checkentry/destroy/me/table/matchsize|
  targetsize/usersize/hooks/proto/family` 的全部偏移.
- ipset 与 netfilter 的 Kbuild 在设备构建传入 `DNSK_XT_NOCOMPAT=1`;
  QEMU/stock GKI 使用默认 compat 构建.
- `dnskctl preflight ipset|netfilter` 从 `policy.conf` 的 `xt_layout`
  (`android16-6.12` 为 104) 传给 `dnsk-preflight`.

## dnsk-preflight

`tools/dnsk_preflight.c` 现在直接解析设备
`/sys/kernel/btf/vmlinux`:

- 校验 BTF header/type/string 边界, 64 MiB 上限;
- 通过分块 `fread` 读取 sysfs (不依赖 `fseek`);
- 找 `struct xt_match`/`xt_target`, 输出
  `btf_layout.xt_match_size/xt_target_size/xt_ok`;
- 指定 `--xt-layout` 时, size 不匹配即 `ok:false` 并返回非零;
- `dnskctl` 只为 ipset/netfilter 传该检查, core/ns/devtmpfs 不受影响.

主机测试 `tests/host/test_preflight.py` 构造合成的 104/128 字节 BTF,
验证正例通过、128 反例失败.

## 证据

- 设备 BTF 解析: 104/104, `xt_ok=true`.
- 修复后的 ipset 真机 `insmod/rmmod` 通过, boot_id 未变, 无 Oops.
- 设备构建的 netfilter 使用同一 shim/断言; active 测试待执行.

## 边界

- 静态断言按目标设备 104 布局; 其他 KMI/厂商 config 必须选择对应构建
  变体并在 profile 中设置 `xt_layout`.
- preflight 当前校验 size; 如需强制字段偏移, 应扩展 BTF member 校验.
