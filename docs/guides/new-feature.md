# 新增一个内核模块 feature

读者: 要给本项目加一个新 feature 的开发者
读完这一篇应当能直接动手,
不需要先去翻 `tools/` 或 `core/` 的实现

## 必须满足的前提

新 feature 先过可行性门, 再写代码

**判定标准是两个层面的语义, 不是"谁在跑这段代码"**:

- **O** -- 加载后**内核态与用户态语义都与原生相同**.判据: 把它换成"打开该
 `CONFIG_*` 重编的内核", 从 `/proc`, `/sys`, 结构布局到 errno, 计数, 都看不出差别
- **V** -- **仅用户态语义相同**(或用户态也只做到部分).用户态看着一样, 但内核里
 没有真实子系统: `/proc/cgroups` 无该行, controller ID 不同, 计数由模块自己维护

**不要用"模块是否复用了内核代码路径"来判断**.一个模块可以调用内核 helper 却仍属 V
(内核没有真正参与那条语义), 也可以只做少量注入却属 O(内核自己的调度/accounting
在跑).完整定义见 `docs/notes/On-OV-split.md`(本地资料).

O 类的额外前提: 确认目标内核缺的是哪个 `CONFIG_*`, 并在
`docs/notes/On-netns-minimum-and-extension.md`(本地资料) 这类清单里登记
「设备已有 / 模块补齐」, 不要重复实现内核已有的能力

## 目录结构

```
features/<name>/
  Kbuild          obj-m := dnsk_<name>.o
  Makefile        外层 make 入口
  main.c          module_init / module_exit
  README.md       feature 自己的 O/V 归属与证据
  ORIGIN.md       若为 vendored 源码，记上游来源；原创则说明
```

`Kbuild` 照抄现有 feature, 关键三行是模块名, `-I$(src)/../../core/include`
和 profile 宏透传

## 代码骨架

契约的唯一权威是 [`core/include/dnsk_core.h`](../../core/include/dnsk_core.h),

| 符号 | 用途 |
|---|---|
| `struct dnsk_core_api` | core 提供的全部能力 (`resolve`, `hook_add/del`, `feature_add/del`, `log`, `patch_pointer`) |
| `struct dnsk_feature_desc` | 注册自己: `name`/`version`/`status`/`caps`/`owner` |
| `dnsk_core_get(abi, size)` | 取得 API, `abi` 必须是 `DNSK_CORE_ABI` |

两条生命周期规则, 做错的后果各不相同, 注意区分:

1. **core 不 pin owner**(注册 feature/hook 不会让核心替你持引用).
 因此 `rmmod` **会成功**, 但模块卸走后 core 里仍留着指向已释放代码的注册项:
 下一次命中该 hook 就跳进已释放内存 -- **UAF / panic**, 而不是卸载失败
 所以 `module_exit()` 必须先 `hook_del()` 每一个 hook, 再 `feature_del()`
2. **`dnsk_core_get()` 成功会取一份 core 的引用**.最后一次使用后必须
 `module_put(api->owner)`, 之后不可再碰 API.漏 put → core 的 refcount 永不归零,
 `rmmod dnsk_core` 永远 EAGAIN; 提前 put 后继续用 → core 可被卸走, feature
 仍在调它的函数表 → UAF

"卸载失败"只由**自持引用**造成 (cgpids native 为了保活槽位自己
`__module_get`), 不是由忘记注销注册项造成

顺序: `module_init` 里先 `dnsk_core_get()`, 再注册 hook, 最后 `feature_add()`;
`module_exit` 严格逆序

现成范例: `features/tmpfsx/main.c`

## 除了代码, 还要登记的地方

漏掉不同位置的**表现差别很大**, 有的会报错, 有的完全静默:

### 构建与打包

| 位置 | 作用 | 漏掉的表现 |
|---|---|---|
| `features/<name>/Kbuild` | `obj-m : = dnsk_<name>.o`, `-I$(src)/../../core/include`, ABI stamp 宏 | 构建报 "No rule to make target"; 或产出缺 stamp, preflight 报 "no dnsk_abi_profile stamp" |
| `features/<name>/Makefile` | 开发者/CI 入口, 转发 `DNSK_*` 变量 | `make -C features/<name>` 直接报 "No targets specified" |
| `.github/workflows/ci.yml` 与 `ci-fast.yml` 的 per-feature 构建清单 | 逐个 `make` + `cp` 到 `build/kmi/<kmi>/` | CI 全绿但 artifact 里没有该 `.ko`; `dnskctl load` 报 "module not shipped in this build" |

`tools/package.py` 用 `glob("dnsk_*.ko")` 打包, **没有 feature 白名单**--
所以"没进 ZIP"的真正原因是 CI 没构建, 不是打包脚本漏了名字
它的 `stamp_keys()` 确实有一份白名单, 只是决定哪些 stamp 要校验

### 用户态入口

`tools/dnskctl` 里有多处按 feature 名硬编码, 漏掉不报错, 只是行为不对:

| 位置 | 作用 | 漏掉的表现 |
|---|---|---|
| `MODULE_ORDER` | `start-all` 的遍历顺序 | **静默跳过**, 不加载也不报错 |
| `stop-all` 的逆序表 | 卸载顺序 | 轮到卸 core 时仍有 feature 在, 报 "feature modules still loaded" |
| `monitor` 的列表 (含 JSON 分支) | 状态展示 | 已加载却显示为 absent |
| `do_profile` | profile 展开 | 任何 profile 都无法包含它 |
| `do_preflight` 的 `case` | 专项 + ABI stamp 校验 | **静默跳过校验**(比拒绝更危险: 错 ABI 的 .ko 也会被放进 insmod) |
| `do_load` 的 `case` | 传 insmod 参数 | 加载成 no-op(缺 `activate=1`) 或选错后端 (缺 `native=0`) |

`load` 本身不需要登记--它按 feature 名拼路径
需要登记的是**参数**

### 其它登记点

| 位置 | 作用 | 漏掉的表现 |
|---|---|---|
| `ksu/webroot/app.js` 的 `MODULES` 数组 | WebUI 模块列表 | WebUI 不显示, 不能操作; CLI 仍正常 |
| `profiles/<kmi>.json` 的 features 字典 | 该 KMI 的状态记录 | 无状态记录; 注意 host 测试只允许 core/ns/devtmpfs 之外写 `not-shipped` |
| `ksu/policy.conf` | 包级 ABI stamp | 仅当引入**新的 ABI knob** 时才需要加 key; 漏则 `package.py` 报 "policy.conf is missing <key>" |

### 文档与验收

| 位置 | 作用 |
|---|---|
| `README.md` 的类别表, `mkdocs.yml` nav, `docs/status.md` | 读者能否找到它 |
| `features/<name>/README.md` 与 `ORIGIN.md` | O/V 归属与上游来源 |
| `tests/device/`, `tests/qemu/init-*`, `qemu-core.yml` 的 mode | 可执行验收证据 |

### 代码契约 (不是"登记", 但漏了就加载不了)

- `#include <dnsk_abi.h>` -- 产生 `MODULE_INFO` 的 ABI stamp
- `MODULE_IMPORT_NS(DROID_LKM_SUITE)` -- `dnsk_core_get` 是
 `EXPORT_SYMBOL_NS_GPL`, 漏则 insmod 报 Unknown symbol

## 验收

- 设备侧: `dnskctl preflight` 通过, `dnskctl load <name>` 成功,
 `dnskctl status` 显示该 feature
- 卸载: `rmmod` 成功, `lsmod` 清洁, 卸载后内核行为回到原生态
- 语义: 见 `docs/notes/On-semantic-differential.md`(本地资料, 不随仓库分发)
