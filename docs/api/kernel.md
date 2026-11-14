# Kernel API

可读版本：[index.html](kernel/index.html) —— 由头文件的 kernel-doc 注释生成，
已提交到仓库，读者不需要自己构建。

## 权威来源

**`core/include/dnsk_core.h` 与 `core/include/dnsk_abi.h` 是唯一定义。**
本页与它们不一致时，以头文件为准，并把这里改对。

生成物只是注释的渲染结果，它不会包含代码里没有的东西，也不会比代码更正确。

## 重新生成

```sh
make docs-kernel KERNEL_DOC=/path/to/scripts/kernel-doc
```

`kernel-doc` 是 Linux 源码树里的一个 Perl 脚本（`scripts/kernel-doc`），
不是系统工具。CI 用 `torvalds/linux` v6.12 的那份；本地可以用 DDK 内核树里的。

生成链路：

```
core/include/*.h  --kernel-doc -rst-->  build/docs/kernel/index.rst
                  --scripts/gen-kernel-api.py-->  docs/api/kernel/index.html
```

## 覆盖范围

| 头文件 | 内容 |
|---|---|
| `dnsk_core.h` | core API、feature/hook 注册契约、生命周期规则 |
| `dnsk_abi.h` | ABI stamp 宏 |
| `dnsk_fs_layout.h` | devtmpfs 用到的结构布局事实 |
| `dnsk_ns_layout.h` | 命名空间相关结构布局事实 |
| `dnsk_xt_layout.h` | xtables match/target 布局选择 |

布局头文件里没有公开的结构体定义（那些结构属于内核），所以它们用
kernel-doc 的 `DOC:` 段说明"这些偏移是什么、为什么需要、什么时候会变"，
而不是逐字段文档化。

## 维护要求

**改了头文件里被注释的部分，就要重新生成并提交这个页面。**
CI 会跑 `make docs-kernel` 然后 `git diff --exit-code docs/api/kernel/index.html`；
页面与头文件不一致时 CI 失败。

这条检查存在的理由：一个过时的生成页面会告诉读者代码已经不再做的事，
比没有文档更糟。