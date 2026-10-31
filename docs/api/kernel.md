# Kernel API

内核侧 API 由 `kernel-doc` 从 `core/include/dnsk_core.h` 与 `core/*.c` 的注释生成，输出在 CI artifact 与 Pages 的 `api/kernel/`。

生成命令（需要 KDIR 提供 `scripts/kernel-doc`）：

```sh
scripts/kernel-doc -html core/include/dnsk_core.h core/*.c > site/api/kernel/index.html
```

当前 ABI 为 2。`dnsk_core_get()` 在成功返回时对 core 取一份模块引用, 调用方必须在最后一次使用后
`module_put(api->owner)`; feature/hook 注册不 pin owner, 对应模块须在 `module_exit()` 中删除注册。
