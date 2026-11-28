# 构建

## 约束

- 本地不进行大体积内核构建; 使用 GitHub Actions 标准 runner + DDK
- 目标 KMI: `android16-6.12`; 其余 7 个 KMI 已由项目所有者搁置, 不参与构建与校验
- 当前为开发阶段, 只产生 artifacts, 不建 tag

## 本地静态检查

```sh
make check
python3 tools/kmi_matrix.py --check
```

`make check` 运行 API 文档一致性, Markdown 风格与 workflow 输入一致性; host 单测用 `python3 -m unittest discover -s tests/host -p 'test_*.py'`. 本机缺少 shellcheck/ruff/clang-format 时对应项输出 `SKIPPED: <tool> missing`, 不声明已检查.

## DDK 构建

```sh
make KDIR=/opt/ddk/kdir/<kmi> M="$PWD" modules
```

CI 使用 `ghcr.io/ylarod/ddk-min: <kmi>-20260828`. `docs-check.yml` 的 `docs` job 在构建前校验 API 文档, 文档风格, workflow 输入一致性与 host 单测; 同文件的 `lint` job 安装并运行 shellcheck (全部 tracked shell, 含无扩展名脚本), ruff (`scripts`/`tools`/`tests`), clang-format (`--dry-run --Werror`, 排除 `vendor/` 与 `selftests/`), yamllint 与 actionlint (workflows), cppcheck (`tools/dnsk_preflight.c`). 仓库不执行 imports guard 或 KSU ZIP 结构检查

## droid-hook 依赖

`dnsk_core.ko` 把 droid-hook 的 `src/*.c` 按路径编入, 不复制源码, 也不使用 submodule
构建 core 前必须有一份 droid-hook checkout:

```sh
# 默认: suite 仓库旁的 sibling checkout
make -C ../droid-hook ...
make KDIR=/opt/ddk/kdir/android16-6.12 M="$PWD" modules

# 或显式指定
make KDIR=/opt/ddk/kdir/android16-6.12 M="$PWD" \
  DNSK_HOOK_DIR=/path/to/droid-hook modules
```

- `DNSK_HOOK_DIR` 的默认值是 `../droid-hook`; 相对路径按 suite 根目录解析, 也可用绝对路径
- 目录缺失或其中没有 `src/dh_hook.c` 时, Kbuild 直接报错并打印期望路径, 不会静默产出
 缺少 inline hook 支持的 core
- 只有 core 构建需要 droid-hook; 各 feature 的 `obj-m` 构建与本地静态检查不需要
- CI 在构建 core 前 checkout `uobe1/droid-hook`, 并用
  `git tag --list --sort=-v:refname | head -n1` 取最新 stable tag 后
  `git checkout --detach`; 构建固定在该 tag 上; 仓库没有 stable tag 时步骤以
  明确错误失败
- stable tag 由 droid-hook 自己的发布工作流在通过门禁的 commit 上创建; suite 的
  构建只能在包含其依赖 API 的 tag 发布之后运行
- 头文件搜索路径由 Kbuild 加 `-I$(DNSK_HOOK_PATH)/include`; 每个 `core/dnsk_dh_*.c`
  是一个按路径 `#include` 对应 droid-hook 源文件的包装 TU, 保留源文件内的 `static` 作用域

## Native oracle

TMPFS/USER_NS/CGROUP_PIDS 的差分测试需要 AOSP 同 KMI 内核:

- 一份开启目标配置 (Native oracle);
- 一份关闭目标配置 (LKM 测试环境);
- 同一 rootfs, 同一测试套件, 同一 QEMU 参数

构建入口在 `.github/workflows/qemu-core.yml`, 差分段在 `tests/qemu/diff_*.sh`, 输入说明在 `ci/qemu/README.md`. 未产生 oracle 的内核不得用于差分结论

## ABI build profile

KMI 只冻结导出符号及其可达类型, 不冻结 config 相关的内部 struct 布局
会影响
模块/内核边界 ABI 的 config 全部记录在 `profiles/abi/*.json` 的 `config`
对象里 (schema 2), 由 `tools/build_flags.py` 校验并转成构建变量:

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

- `config` 必须包含 registry 里的全部 key; 缺失或未知 key 直接报错, 不回退
 默认值
 只有能表达成代码宏的 knob 会输出构建变量; 锁/RT/KASAN/cgroup 等
 必须选择 config 匹配的内核/DDK 构建, 不能用头文件 shim 伪装
- `android16-6.12-xt104`: 设备/vendor 变体, `CONFIG_NETFILTER_XTABLES_COMPAT=n`,
 `xt_match/xt_target` 104 字节; 默认设备构建
- `android16-6.12-xt128`: stock GKI/DDK 6.12, compat 开启, 128 字节; QEMU 使用
- CI 矩阵只在 `android16-6.12` 条目上应用 profile, 并用 `--kmi` 校验 profile
 与构建 KMI 一致; 其他 KMI 不再误用 6.12 profile
- 模块内嵌 `dnsk_abi_profile=` 与 `dnsk_xt_layout=` stamp; KSU 包的
 `policy.conf` 写入同名的 `abi_profile`/`xt_layout`, `dnskctl load` 先比对
 stamp, 再用 `dnsk-preflight` 校验设备 BTF, 全部通过才允许 `dnskctl load`
- 新增会影响模块/内核边界 struct 的 config 时, 必须同时更新 profile
 registry, 构建变体和 BTF preflight 检查; 不能仅靠 `uname` KMI 判断
