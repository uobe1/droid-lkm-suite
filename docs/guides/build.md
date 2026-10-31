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
