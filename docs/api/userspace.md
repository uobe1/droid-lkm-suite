# Userspace API

用户态 C 与 `dnskctl` 的接口由 Doxygen 生成（`Doxyfile`），输出在 CI artifact 与 Pages 的 `api/userspace/`。

```sh
doxygen Doxyfile
```

`dnskctl` 是唯一受支持的加载入口；`dnsk-preflight` 输出 JSON 事实，不伪造成功。
