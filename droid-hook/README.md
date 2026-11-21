# Droid-Hook

状态: **库层 + 运行时适配已实现, host 侧 75 项单测通过, 未接入内核构建, 未门控**.
本目录不进任何 `features/*/Kbuild`, 不改变 `ci-fast` 的产物清单, 也不影响任何现有 LKM 的行为.

## 这是什么

Droid-Hook 是一个可嵌入 LKM(或其它内核模块)的 Patch / Hook / RW 库, 加一层可替换的
运行时. 路线由用户 2026-11-14 授权并写入设计证据:
以本仓库 core 的 RW ABI 5 单入口 `rw` / `patch_pointer` 为能力核, 综合 KernelPatch 的
hotpatch / inline hook / kstorage 与上游 livepatch / static_call / text_poke 等设计,
自行实现库 + 运行时, **不引入 KernelPatch 作为第二套 root 栈**.

## 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 库层 `libdroidhook` | `include/dh.h`, `src/` | 只依赖注入的 `struct dh_backend`, 不持有全局状态, 不 include core 头 |
| 运行时 `dh_runtime` | `runtime/` | 把 core RW ABI 5 适配成 backend; 换载体 = 换 backend 实例, 库层零改动 |

## 已实现(host 侧验证)

| 文件 | 职责 | 关键点 |
|---|---|---|
| `src/dh_rw.c` | ctx 生命周期与 RW 包装 | 256B/页切分; 每次 WRITE 读回校验; 8 字节对齐检查; backend 缺失或 abi 不符返回 `DH_E_INVAL` |
| `src/dh_txn.c` | 事务与回滚 | undo 静态 8 槽零分配; 存原字节; 逆序 replay; 满槽 `DH_E_NOSPC`; 读回不一致 `DH_E_VERIFY` 并让 commit 自动回滚; 回滚在单个 quiesce 窗口内 |
| `src/dh_insn.c` | arm64 编码与引用重定位 | 绝对跳转 `LDR X17,#8; BR X17;<literal>`; ADRP/ADR/B/BL/B.cond/CBZ/TBZ/LDR literal 重编码; 成对 ADRP+ADD/LDR 重定位(fail-closed); ±4GiB 与 ±1MiB 范围检查 |
| `src/dh_hook.c` | 函数级 trampoline | 覆盖前 4 条指令 + relo 区 + 跳回第 5 条; key 与 target 双重唯一; 地址仲裁; before 正序 / after 逆序 / skip_origin; 卸载逆序且在 quiesce 窗口内 |
| `src/dh_shadow.c` | `<obj,id>` 侧表 | 不改父对象 layout; get-or-alloc 幂等; 延迟回收至最后一个引用 |
| `runtime/dh_core_backend.c` | ABI 5 适配 | `rw`/`patch_pointer`/`resolve` 映射; magic/abi 校验; `F_TEXT`/`F_RO` 走会睡眠的 patch 后端; 数据路径 `flags=0` 不进 stop_machine |

### 指令编码的两个实证结论

- `ADD`(immediate) 的 imm12 在 bits[21:10], `sh` 在 bit22, 正确掩码是 `0x003FFC00`.
  `0x00FFFC00` 覆盖 bits[10:23], 会把 `sh` 与 bit23 当数据位; 旧原型靠"整片取出再原样放回"
  的巧合才没出错. 本库按 ARM ARM 拆分解码/编码, 并由 `test_add_imm12_and_sh_preserved`
  与 `test_pair_add_rewrites_imm12_and_keeps_sh` 锁死.
- `LDR`/`STR` unsigned-offset 的 `size`/`opc` 位必须保留, 且 imm12 按访问宽度缩放;
  对应 `test_ldr_opc_bits_not_cleared` 与 `test_pair_ldr_keeps_size_opc_and_rescales_imm12`.

## 验证

- 6 个 TU 在生产参数(`-std=c11 -Wall -Wextra -Werror`, 不含测试 seam)下零 warning 编译
- 5 套 host 单测共 **75 项**: contract 6 / rw+txn 12 / insn 19 / hook 19 / runtime 19
- 关键断言做过变异验证(故意改坏实现, 测试必须失败): hook 侧 4 个变异全部被捕获,
  insn 侧 2 个变异(掩码回退、删除范围检查)全部被捕获, 证明断言不是空转
- 测试 seam `-DDH_INS_TEST_SEAM` 只在测试构建定义; 生产目标文件已用 `nm` 确认不含该符号

## 明确未做(不要误读为已完成)

- 没有 `Kbuild`, 没有内核模块编译, **没有 QEMU 门控, 没有真机加载**
- 4 条 RO 写入路由(ALIAS / PTE / FIXMAP / DMA)目前只有 `FIXMAP` 经 core 的
  `F_RO` 后端真实存在; 其余三条是契约占位, 适配层尚未区分
- `dh_insn` 的成对重编码是内部助手 + `dh_scan_refs` 的 fail-closed 预检,
  **没有公开入口把重编码后的指令交给调用方**(需要改 `dh.h`, 属 ABI 变更)
- `dh_hook_install` 固定按 4 条原始指令预留 relo 槽, 依赖
  "relocate 不会把一条指令展开成多条"; 若将来支持 MOVZ/MOVK 序列, 需要
  `dh.h` 增加 out-len 参数
- 运行时适配用固定 4 槽 trampoline 解决"backend 回调无 userdata"的多实例隔离,
  超过 4 个 backend 实例会返回 `DH_E_NOSPC`
- 设计证据里的 6 个待决定点(KPM 迁移节奏, 运行时载体, 许可证边界, EL2 面策略,
  force 卸载语义, 性能基线口径)仍待用户决定

## 已知的同类隐患(不在本目录, 未改)

`features/cgpids/native.c` 的 ADD 分支同样使用 `0x00FFFC00`. 它靠"先清该字段再写低 12 位"
得到正确结果, 行为正确, 但把 `sh` 与 opcode 位当数据位处理, 属同类 latent hazard.
那是已门控的生产代码, 本轮不动.

## 设计证据与计划

- `docs/evidence/droid-hook-design-research.md`(本地资料, 945 行, 89 条一手 URL)
- `docs/plans/optional-future-2026-11-14.md` §4
- `docs/evidence/rw-memory-mutation-research.md`(RW 扩容/改值配方与 prior art)

## 本地验证

```sh
make check                  # 顶层, 含本目录的 host 统一门禁
make -C droid-hook check    # 只跑本目录: 逐个编译每个 TU + 全部 host 单测
make -C droid-hook clean
```

`droid-hook/Makefile` 是 host 侧编译门禁, 不是内核模块构建: 每个 `.c` 单独
`-c` 过类型检查, 不链接(各 harness 自带 stub, 全链接会混用冲突 stub).
