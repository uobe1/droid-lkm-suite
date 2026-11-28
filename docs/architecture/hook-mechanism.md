# Hook 机制

读者: 需要为 feature 选择 hook 机制, 并确保注销后可以安全卸载的开发者

读完之后应能判断该用 inline hook 还是 `patch_pointer`, 以及删除时为什么必须等 RCU

## 两种受管机制

core 只提供两种 hook 机制, 没有 kprobe/kretprobe/syscall 槽类型:

| 机制 | 改什么 | 谁安装 | 谁保存原值 | 适合 |
|---|---|---|---|---|
| inline hook | 在 `symbol` 入口写入跳转, 调用 `replacement` | core (`hook_install()`) | core 生成 origin 块 | 需要介入函数入口或返回值 |
| data-slot patch | 改写一个指针槽 | feature (`patch_pointer()`) | feature 自己 | 只替换 ops 表项/函数指针, 不改指令 |

若内核已经提供正式注册接口(nft/xt/ipset, cgroup cftypes, pernet 等), 优先直接注册;
这是最稳的路径, 失败时按逆序回滚, 不涉及代码改写

深移植 feature (`ns`, `devtmpfs`) 另自带入口指令替换, 由 feature 自己负责 quiesce 与恢复;
规则见 [卸载安全](unload-safety.md)

## inline hook

### 请求

`hook_install()` 接受 `struct dnsk_inline_hook_req`:

| 字段 | 含义 | 要求 |
|---|---|---|
| `key` | 注销凭据 | 必填, 短于 `DNSK_HOOK_KEY_LEN`(48, 含 NUL), 同 key 重复返回 `-EBUSY` |
| `symbol` | 被 hook 的内核符号名 | 必填, 经 kallsyms 解析; 解析失败返回 `-ENOENT` |
| `owner` | 注册模块 | 必填, 只用于诊断; core 不 pin |
| `replacement` | 同原型替换函数 | 必填; 原型必须与 `symbol` 完全一致 |
| `origin_out` | 可选的原函数块输出 | 非 NULL 时安装失败即 fail-closed; NULL 表示完全替换 |

`replacement` 必须按目标原型声明参数与返回类型, 不能使用 `void *` 间接调用

要保留原逻辑, `replacement` 调用 `*origin_out` 收到的块; 该块是 droid-hook 重定位后的
可执行原函数, 不是目标地址, 也不是可随意转存的普通函数指针

`origin_out` 收到的块只在 `hook_uninstall(key)` 成功返回前有效; 卸载后不得再调用

### 安装语义

1. `hook_install()` 解析 `symbol`, 并把请求交给 droid-hook
2. droid-hook 在 `module_alloc` 内存里构建 origin 块; 该分配器经 kallsyms 解析
3. 需要 origin 时, 库先把可调用块的地址写入 `*origin_out`; 没有可用
   `alloc_exec` 时在写入口之前返回 `-EOPNOTSUPP`
4. origin 写好后, 库才在入口写入跳转并读回校验
5. 入口写入之前的失败会清空 `*origin_out`; 入口写入已经发出后的失败会回滚入口,
   但不释放 origin 块--该块保持有效, 以免在途 CPU 取指到已释放内存, 此时
   `*origin_out` 仍指向该块, 而 hook 记录不填充

origin 指针因此先于入口生效, replacement 不需要处理"入口已生效但 origin 仍为空"
的安装窗口. 保留 NULL 检查只是防御: 若某次安装异常地发布了没有 origin 的入口,
该次调用 fail-closed, 而不是解引用 NULL:

```c
static void *dnsk_origin;

static __nocfi int replacement(struct thing *t)
{
	int (*origin)(struct thing *) = READ_ONCE(dnsk_origin);

	if (unlikely(!origin))
		return -EAGAIN;
	pre_work(t);
	return origin(t);
}
```

origin 是重定位后的机器码, 没有 kCFI 类型哈希; 对它的普通间接调用会被 CFI 检查
拒绝并进入 `__cfi_check_fail`. 因此 replacement 必须以 `__nocfi` 编译 (或在那一处
调用自行抑制检查); 它的原型仍必须与目标完全一致

void 原型则跳过原逻辑并返回

### 卸载语义

- `hook_uninstall(key)` 恢复入口原字节; 恢复成功后再等一个 RCU 宽限期才返回
- 只有恢复被证明成功才删除注册项; 恢复失败返回错误并保留该 hook, 调用方不得释放
  handler 数据, 也不得把状态标记为已恢复
- `key` 为 NULL 返回 `-EINVAL`, 无此 key 返回 `-ENOENT`
- origin 可执行块在卸载后**退役, 不释放**: 入口恢复只阻止新调用者, 无法证明没有 task
  已在 origin 块内; 释放会重演模块 text 的 use-after-free. 代价是每次 hook 加载泄漏
  一个可执行块, 见 [卸载安全](unload-safety.md)

`module_exit()` 是 void 函数, 不能因为恢复失败而中止卸载
可失败的拒绝必须在 rmmod 之前完成; exit 里遇到恢复失败只能记录, 并保持模块 text
仍可达这一事实

### `synchronize_rcu()` 的作用

`stop_machine()` 只保证"恢复入口的那一刻, 没有 CPU 正停在被 hook 的函数里"

它不能保证一个在恢复前刚进入 replacement 的 CPU 已经离开模块 text, 也不能保证
origin 块的调用者已经返回

因此 `hook_uninstall()` 在恢复原值之后, 返回之前调用 `synchronize_rcu()`

效果是:

- 调用方在返回后可以立即释放 handler 数据, 不会再有 RCU 读侧临界区引用它
- 卸载路径不会与"刚进入 replacement 的调用者"竞争
- 代价是 rmmod 路径最多可能阻塞几十毫秒, 这是可接受的: rmmod 不是热路径

`synchronize_rcu()` 不覆盖 task_work, RCU 回调, superblock 销毁等**在模块退出之后
才运行**的路径; 后者见 [卸载安全](unload-safety.md)

## data-slot patch

不是所有 hook 都需要改指令. 只替换 ops 表里一个函数指针时, 用 `patch_pointer()`:

```c
static struct {
	const char *key;
	void **slot;
	void *orig;
	bool ready;
} dnsk_slots[16];
static int dnsk_nr;

static int patch_slot(const char *key, void **slot, void *value)
{
	void *orig = READ_ONCE(*slot);
	int ret = dnsk_api->patch_pointer(slot, value);

	if (ret)
		return ret;
	dnsk_slots[dnsk_nr].key = key;
	dnsk_slots[dnsk_nr].slot = slot;
	dnsk_slots[dnsk_nr].orig = orig;
	dnsk_slots[dnsk_nr].ready = true;
	dnsk_nr++;
	return 0;
}
```

- 原值允许为 NULL(例如内核编译时缺失的 inode operation 槽); 恢复时把 NULL 原样写回
- 写入走 patch 后端(页表遍历 + fixmap + `stop_machine`), 因此 init 后只读的槽也能改
- core 不保存槽位, 只提供写入原语; feature 必须在 exit 里按安装逆序逐个恢复
- 恢复失败的槽保持"未恢复"记录, 不能标记为已恢复; 下一次命中仍会跳进模块 text

`patch_pointer()` 不是 inline hook, 不产生 origin, 也不经过 droid-hook 注册表

## 怎么选

1. **内核已有注册接口** → 直接注册, 不要为了统一而套 hook
2. **需要读取或改写函数返回值**, 或需要围绕函数体做事 → inline hook;
   replacement 调 origin 后处理返回值
3. **只需替换 ops 表里的一个函数指针** → data-slot patch; 不要用 inline hook
   去模拟一次指针替换
4. **高频路径的 syscall 表项** → data-slot patch 一个槽, 并在 preflight 核对槽原值;
   槽已被别人改过时直接覆盖会丢失对方的 hook
5. **只想观察一个低频入口而不改语义** → inline hook, replacement 直接调用 origin;
   不需要返回值处理时也可用 `rw` 的 PROBE 只做寻址

## 注册与删除的生命周期

- 在 `module_init()` 里, `dnsk_core_get()` 成功之后安装; 在 `feature_add()` 之前完成,
  保证 `dnskctl status` 看到的 feature 已经可用
- key 是删除时的唯一凭据, 必须稳定; data-slot 的 key 只用于本 feature 的记录,
  不进入 core 注册表
- `owner` 必填但只用于诊断; core 不在 owner 上取引用, 因此不会替 feature 保活或自动注销
- 带着 active hook 卸载 feature, core 注册表或指针槽仍指向已释放的模块 text--
  下一次命中就是 use-after-free
- 删除顺序与注册相反; inline hook 先恢复入口, data-slot 后恢复槽, 最后 `feature_del()`
  与 `module_put()`
- 任何一步失败都要逆序回滚已经成功的安装; 失败路径不能留下半套 hook

## 后端映射

core 把 suite 现有原语适配给 droid-hook 的 `struct dh_backend`:

| 后端槽 | suite 实现 |
|---|---|
| `rw` | `dnsk_rw_op()`, 逐字段拷贝请求 |
| `patch_pointer` | `dnsk_patch_pointer()` |
| `text_write` / `ro_write` | `dnsk_patch_text()`, 固定 `flush_icache=true` |
| `probe` / `icache_flush` | `dnsk_resolve_symbol()` / 已解析的 cache 刷新函数 |
| `alloc` / `free` | `kmalloc()` / `kfree()` |
| `alloc_exec` / `free_exec` | 经 kallsyms 解析的 `module_alloc` / 退役不释放 |
| `tlb_flush` | 空操作; fixmap 写入不改变 PTE 映射 |
| `quiesce_begin` / `quiesce_end` | 空操作; 原子窗口由每次 `text_write` 的 `stop_machine` 提供 |

`alloc_exec` 解析不到时保持 NULL, 需要 origin 的安装因此 fail-closed

## 自查清单

- [ ] 先问有没有正式注册接口; 有就直接注册, 不套 hook
- [ ] 需要函数体介入或返回值处理用 inline hook; 只换指针用 `patch_pointer`
- [ ] replacement 的原型与目标完全一致, 且调用 origin 继续原逻辑
- [ ] replacement 在 origin 尚未发布时返回错误或跳过, 不解引用 NULL
- [ ] 每个 hook 有唯一, 稳定, 短于 48 字节的 key
- [ ] 安装数量受 droid-hook 注册表容量限制; 表满返回 `-ENOSPC`, 不要把 hook
 数量当作无限
- [ ] 注册在 `core_get` 之后, `feature_add` 之前; 删除在 `module_put` 之前,
 且逆序
- [ ] `hook_uninstall()` 成功返回前不释放 handler 数据; 返回后才释放
- [ ] 恢复失败不标记已恢复; 不依赖 core 自动清理 hook; core 不 pin owner
- [ ] 若 hook 会触发延迟执行的回调, 按卸载安全检查表处理, 而不是只加
 `synchronize_rcu()`

## 相关阅读

- [core API 契约](../../core/include/dnsk_core.h): `struct dnsk_inline_hook_req`,
 `hook_install()` 与 `hook_uninstall()` 的字段与错误语义
- [卸载安全](unload-safety.md): 延迟回调, origin 退役与 module text 的关系
- [模块生命周期](module-lifecycle.md): 注册顺序与引用计数责任
