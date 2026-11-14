# Droid-Hook

状态: **库层契约骨架, 未接入内核构建, 未门控**. 本目录不进任何 `features/*/Kbuild`,
不改变 `ci-fast` 的产物清单, 也不影响任何现有 LKM 的行为.

## 这是什么

Droid-Hook 是一个可嵌入 LKM(或其它内核模块)的 Patch / Hook / RW 库, 加一层可替换的
运行时. 路线由用户 2026-11-14 授权并写入设计证据:
以本仓库 core 的 RW ABI 5 单入口 `rw` / `patch_pointer` 为能力核, 综合 KernelPatch 的
hotpatch / inline hook / kstorage 与上游 livepatch / static_call / text_poke 等设计,
自行实现库 + 运行时, **不引入 KernelPatch 作为第二套 root 栈**.

## 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 库层 `libdroidhook` | `include/dh.h`, 后续 `src/` | 只依赖注入的 `struct dh_backend`, 不持有全局状态, 不 include core 头 |
| 运行时 `dh_runtime` | 后续 `runtime/` | 默认由 `dnsk_core.ko` 提供 backend, 也可换其它载体; 换运行时 = 换 backend 实例, 库层零改动 |

## 本轮已落地

- `include/dh.h`: 公开契约. 8 个核心结构, 4 条 RO 写入路由(ALIAS / PTE / FIXMAP / DMA),
  事务化回滚(固定 8 槽 undo, panic 路径不分配), hook / shadow / preflight / quiesce
  的 API 草案, 与现有 KPROBE / SYSCALL / DATA / KRETPROBE 的地址仲裁接口
- `tests/host/dh_header_probe.c` + `test_dh_contract.py`: 用本机 `cc` 在**无内核头**下
  编译并运行, 断言 ABI 常量, 结构布局, 字段偏移, 以及"每个公开声明都有 kernel-doc 注释"
- 该自测已纳入 `make check` 与 `.github/workflows/docs-check.yml`
- `dh.h` 的 15 个公开声明已纳入 `scripts/gen-api-docs.py` 的注释唯一来源与覆盖审计,
  生成到 `docs/api/kernel.md`

## 明确未做(不要误读为已完成)

- 没有任何 `dh_*` 函数实现, `src/` 与 `runtime/` 尚不存在
- 没有 `Kbuild`, 没有内核模块编译, 没有 QEMU 门控, 没有真机加载
- 设计证据里的 6 个待决定点(KPM 迁移节奏, 运行时载体, 许可证边界, EL2 面策略,
  force 卸载语义, 性能基线口径)仍待用户决定

## 设计证据与计划

- `docs/evidence/droid-hook-design-research.md`(本地资料, 945 行, 89 条一手 URL)
- `docs/plans/optional-future-2026-11-14.md` §4
- `docs/evidence/rw-memory-mutation-research.md`(RW 扩容/改值配方与 prior art)

## 本地验证

```sh
make check                                    # 顶层, 含本目录的 host 契约自测
cd droid-hook/tests/host && python3 -m unittest test_dh_contract.py
```
