# Kernel API

内核侧 API 由 `kernel-doc` 从 `core/include/dnsk_core.h` 与 `core/*.c` 的注释生成，输出在 CI artifact 与 Pages 的 `api/kernel/`。

生成命令（需要 KDIR 提供 `scripts/kernel-doc`）：

```sh
scripts/kernel-doc -html core/include/dnsk_core.h core/*.c > site/api/kernel/index.html
```

当前 M0 尚未发布可用的 feature API；页面只描述 core ABI 与稳定性规则。
