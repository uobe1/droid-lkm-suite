# 模块生命周期

读者: 新增或修改 feature 的开发者
读完应能写出正确的 `module_init`/
`module_exit`, 并知道失败时按什么顺序回滚

## 加载顺序

- 默认只自动加载 `dnsk_core.ko`; feature 由 `dnskctl`/KSU WebUI 选择 profile 后
 按需 `ksud insmod`
- 顺序固定为 **core 先, feature 后**.feature 在 `module_init` 里调用
 `dnsk_core_get()`, core 未就绪返回 `-ENODEV`, ABI 或 `size` 不匹配返回
 `-EPROTO`; 两者都不能继续
- `dnskctl` 的 `MODULE_ORDER` 决定实际加载顺序
- 新增 feature 必须登记; 漏掉会
 表现为加载顺序错误
- 加载前跑 preflight: KMI/vermagic, 4K 页, BTF layout, 符号, 入口指纹, KCFI,
 syscall 槽原值
 任一失败 fail-closed, 不允许"先加载再观察"

## `dnsk_core_get` / `module_put` 的引用计数责任

`dnsk_core_get(abi, size)` 成功时会在 core 模块上取一份引用, 并返回
`struct dnsk_core_api *`.调用方的责任是:

1. 在 API 可能被使用的整个期间持有这份引用;
2. 最后一次使用后调用 `module_put(api->owner)`; **put 之后不得再触碰 API**,
 表本身随 core 卸载失效

一次 `dnsk_core_get()` 必须对应一次 `module_put()`, 包括提前返回的路径: 检测到
内核已有原生能力而主动拒绝, 以 `keep_native` 选择"加载但不接管", 初始化中途
任一步失败
漏掉 put 的后果不是"多占一点内存", 而是 **core 永远无法卸载**:
feature 持有 core 引用, core 的 refcount 不归零, `rmmod dnsk_core` 永远
`EAGAIN`.`dnsk_cgpids` native 分支的提前 return 就是漏掉 put 的典型位置

## feature 注册与反注册的对称性

`feature_add()` 只做登记, 不启用, 不安装任何东西:

- `name` 必填且短于 `DNSK_FEATURE_NAME_LEN`(32, 含 NUL); `owner` 必填;
- 名字重复返回 `-EEXIST`, 注册表满返回 `-ENOSPC`, 缺 `name`/`owner` 返回
 `-EINVAL`;
- `name`, `version`, `caps` 都由 core 按值拷贝, 调用方的字符串不必继续存活

每个 `feature_add()` 必须在 `module_exit()` 里有对应的 `feature_del(name)`
core 不监视 feature 模块的卸载, 也不会替你删掉注册行; 不删就会留下一条属于已
消失模块的记录, `dnskctl status` 会继续报告一个不存在的 feature

## hook 注册与删除的对称性

每个 `hook_add()` 必须在 `module_exit()` 里用**同一个 key** 调用 `hook_del()`:

- key 必填且短于 `DNSK_HOOK_KEY_LEN`(48, 含 NUL); 重复 key 返回 `-EBUSY`,
 hook 表满返回 `-ENOSPC`, 请求不合法返回 `-EINVAL`, patching 后端不可用返回
 `-EOPNOTSUPP`;
- `hook_del()` 恢复被替换的原值, 成功后再等一个 RCU 宽限期才返回; 返回后调用
 方可以释放 handler 数据;
- 删除失败返回 `-EINVAL`(key 为 NULL), `-ENOENT`(无此 key) 或恢复原值错误

hook 不是"注册完就归 core 管".core 只保存注册项并在 `dnsk_hook_del()` 时恢复
原值; 它不会在 feature 卸载时替你删 hook.带着 active hook 卸载模块, 等于把
core 注册表里的 handler 指针留在已释放的模块 text 上

## core 不 pin owner 的含义

`struct dnsk_hook_req.owner` 和 `struct dnsk_feature_desc.owner` 都只用于诊断,
core **不在这两个 owner 上取模块引用**.两个必须理解的结果:

- core 不会阻止 feature 卸载, 也没有能力在 feature 卸载时自动清理
- 安全卸载的
 责任完全在 feature: 清掉每个 hook, 删掉 feature 注册, 归还 core 引用
- "让 core pin 住 feature 来保证回调有效"是错误方向.pin 会让 feature 的
 refcount 永远不归零, `module_exit` 不运行, 反注册也就永远不会发生--这是
 自锁, 不是保护.`dnsk_cgpids` 的自持引用就是这种死锁的实例: `rmmod` 永远
 `EAGAIN`, 而唯一会释放引用的 `module_exit` 永远不会被调用

正确方向相反: feature 持有 core(保证 API 有效), core 不持有 feature

## `module_init` / `module_exit` 的固定顺序

```
static int __init x_init(void)
{
        api = dnsk_core_get(DNSK_CORE_ABI, sizeof(*api));
        if (IS_ERR(api))
                return PTR_ERR(api);
        ret = resolve_symbols();          /* 失败 -> out_api */
        ret = install_hooks();            /* 失败 -> 逆序删已装 hook */
        ret = api->feature_add(&desc);    /* 失败 -> 删 hook -> out_api */
        return 0;
out_api:
        module_put(api->owner);
        return ret;
}
static void __exit x_exit(void)
{
        if (feature_ready)  api->feature_del("x");
        remove_hooks_in_reverse_order();
        module_put(api->owner);
        release_private_state();
}
```

两条硬规则:

- **init 正序, exit 严格逆序**.exit 先删 feature 注册, 再删 hook, 最后 put
 core; 私有状态在所有 hook 和注册都消失后释放
- **`module_exit` 不能失败**.可能拒绝的操作 (如仍有活跃对象时返回 `-EBUSY`) 必须
 在 rmmod 之前由可失败的入口完成, 例如 module parameter 的 write 或
 `dnskctl unload` 预检; 塞进 exit 只会把可恢复错误变成静默的 use-after-free

## 新增 feature 自查清单

- [ ] init 第一步 `dnsk_core_get()`, 最后一步 `feature_add()`; exit 严格逆序
- [ ] 每条错误路径都回滚已完成步骤, 包括提前返回和"保持原生"路径
- [ ] 每个 `feature_add()`/`hook_add()` 都有对应删除, `get`/`put` 严格成对
- [ ] 退出路径不依赖"core 会帮我清理": core 不 pin owner, 也没有卸载回调
- [ ] 可失败的操作已前移到 rmmod 之前; `module_exit` 只做确定的清理

## 相关阅读

- [core API 契约](../../core/include/dnsk_core.h): 引用计数, 注册/注销与错误码
- [卸载安全](unload-safety.md): 退出路径上哪些对象会让模块无法安全离开
- [新增 feature 指南](../guides/new-feature.md): 登记点与验收流程
