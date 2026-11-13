# Kernel API

**状态：本文件是手写的接口契约，不是生成产物。**

仓库里没有启用 `kernel-doc` 生成流程（`docs.yml` 不运行它，也没有 periodic 任务）。
早先版本声称"由 kernel-doc 生成到 CI artifact"，但那条流水线从未存在——读到这里时
不要去找那个 artifact。

权威定义永远是 `core/include/dnsk_core.h`；本文件若与它不一致，以头文件为准，
并把这里修正。

当前 ABI 为 **3**（`DNSK_CORE_ABI 3`；ABI 3 相比 2 增加了 `patch_pointer`）。`dnsk_core_get()` 在成功返回时对 core 取一份模块引用, 调用方必须在最后一次使用后
`module_put(api->owner)`; feature/hook 注册不 pin owner, 对应模块须在 `module_exit()` 中删除注册。
