# 构建

## 约束

- 本地不进行大体积内核构建；使用 GitHub Actions 标准 runner + DDK。
- 目标 KMI：`android12-5.10`、`android13-5.10`、`android13-5.15`、`android14-5.15`、`android14-6.1`、`android15-6.6`、`android16-6.12`、`android17-6.18`。
- 首 tag 需要全部 KMI 通过；开发版本 `vX.Y.ZdevN` 只产生 artifacts，不建 tag。

## 本地静态检查

```sh
make check
python3 tools/kmi_matrix.py --check
```

## DDK 构建

```sh
make KDIR=/opt/ddk/kdir/<kmi> M="$PWD" modules
```

CI 使用 `ghcr.io/ylarod/ddk-min:<kmi>-20260828`，并在构建前执行 imports guard、KSU ZIP 结构检查、文档构建。

## Native oracle

TMPFS/USER_NS/CGROUP_PIDS 的差分测试需要 AOSP 同 KMI 内核：

- 一份开启目标配置（Native oracle）；
- 一份关闭目标配置（LKM 测试环境）；
- 同一 rootfs、同一测试套件、同一 QEMU 参数。

构建与运行脚本在 `ci/qemu/` 与 `tests/differential/`。未产生 oracle 的内核不得用于差分结论。

## ABI build profile

KMI 只冻结导出符号及其可达类型，不冻结 config 相关的内部 struct 布局。会影响
模块/内核边界 ABI 的 config 全部记录在 `profiles/abi/*.json` 的 `config`
对象里（schema 2），由 `tools/build_flags.py` 校验并转成构建变量：

```sh
python3 tools/build_flags.py --profile profiles/abi/android16-6.12-xt104.json \
  --kmi android16-6.12
# DNSK_ABI_PROFILE=android16-6.12-xt104
# DNSK_XT_NOCOMPAT=1
# DNSK_XT_LAYOUT=104
# DNSK_PAGE_SIZE=4096
# DNSK_MAX_SKB_FRAGS=17
# DNSK_CGROUP_SUBSYS_COUNT=8
```

- `config` 必须包含 registry 里的全部 key；缺失或未知 key 直接报错，不回退
  默认值。只有能表达成代码宏的 knob 会输出构建变量；锁/RT/KASAN/cgroup 等
  必须选择 config 匹配的内核/DDK 构建，不能用头文件 shim 伪装。
- `android16-6.12-xt104`：设备/vendor 变体，`CONFIG_NETFILTER_XTABLES_COMPAT=n`，
  `xt_match/xt_target` 104 字节；默认设备构建。
- `android16-6.12-xt128`：stock GKI/DDK 6.12，compat 开启，128 字节；QEMU 使用。
- CI 矩阵只在 `android16-6.12` 条目上应用 profile，并用 `--kmi` 校验 profile
  与构建 KMI 一致；其他 KMI 不再误用 6.12 profile。
- 模块内嵌 `dnsk_abi_profile=` 与 `dnsk_xt_layout=` stamp；KSU 包的
  `policy.conf` 写入同名的 `abi_profile`/`xt_layout`，`dnskctl load` 先比对
  stamp，再用 `dnsk-preflight` 校验设备 BTF，全部通过才允许 `ksud insmod`。
- 新增会影响模块/内核边界 struct 的 config 时，必须同时更新 profile
  registry、构建变体和 BTF preflight 检查；不能仅靠 `uname` KMI 判断。
