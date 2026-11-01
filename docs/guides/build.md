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

KMI 只冻结导出符号，不冻结 config 相关的内部 struct 布局。会影响设备 ABI 的
config 必须放进 `profiles/abi/*.json`，由 `tools/build_flags.py` 转成构建变量：

```sh
python3 tools/build_flags.py --profile profiles/abi/android16-6.12-xt104.json
# DNSK_BUILD_PROFILE=android16-6.12-xt104
# DNSK_XT_NOCOMPAT=1
# DNSK_XT_LAYOUT=104
```

- `android16-6.12-xt104`：设备/vendor 变体，`CONFIG_NETFILTER_XTABLES_COMPAT=n`，
  `xt_match/xt_target` 104 字节；默认设备构建。
- `android16-6.12-xt128`：stock GKI 6.12，compat 开启，128 字节。
- CI `workflow_dispatch` 的 `build_profile` 输入选择 profile；`push` 构建默认
  xt104。KSU 包的 `policy.conf` 会写入对应的 `xt_layout`，`dnskctl load`
  先用 `dnsk-preflight` 校验设备 BTF，再允许 `ksud insmod`。
- 新增会影响模块/内核边界 struct 的 config 时，必须增加 profile 字段、构建
  变体和 BTF preflight 检查；不能仅靠 `uname` KMI 判断。
