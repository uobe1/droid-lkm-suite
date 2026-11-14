# Userspace API

用户态 C 接口由 Doxygen 生成，流水线见 `.github/workflows/docs.yml`：
`doxygen Doxyfile` 输出到 `build/docs/doxygen-userspace/html/`，再复制进
`site/api/userspace/` 随 Pages 发布。

```sh
doxygen Doxyfile   # 产物在 build/docs/doxygen-userspace/html/
```

因此**这个页面是生成产物的入口说明，不是 API 手册本身**——真实的函数签名看
生成结果或直接看源码。

## 两个入口

| 入口 | 形态 | 用途 |
|---|---|---|
| `dnskctl` | POSIX shell（`tools/dnskctl`） | 设备侧唯一受支持的入口：加载、卸载、状态、诊断、profile |
| `dnsk-preflight` | 静态 aarch64 二进制（`tools/dnsk_preflight.c`） | 加载前的事实校验，输出单行 JSON |

`dnskctl` 在加载前一定先跑 `dnsk-preflight`，preflight 失败则拒绝加载——这是
"不伪造成功"约束的落点。

## dnskctl 约定

- 所有结果来自真实系统读取，不缓冲、不美化。
- `--json` 输出单行 JSON 对象；退出码仍反映错误。
- 错误写到 stderr，正常结果写到 stdout。

完整命令列表见 `dnskctl help`。命令的实现是权威，本文件只描述契约。
