# cgpids v2 differential (QEMU)

同一份 `tests/qemu/init-cgpids` 在两个内核上运行：

- `kernel=oracle`：DDK 6.12.76，`CONFIG_CGROUP_PIDS=y`（Native）
- `kernel=oracle-nopids`：同一 DDK 源码，`CGROUP_PIDS=n`、`CGROUP_DEVICE=n`，
  initramfs 先 `insmod dnsk_core.ko` + `dnsk_cgpids.ko`（Virtual）

## Native baseline (run 42216318718)

```text
DNSK_CGPIDS_CONTROLLERS=cpuset cpu io memory pids
DNSK_CGPIDS_ROOT_SUBTREE=pids
DNSK_CGPIDS_MAX_BEFORE=max
DNSK_CGPIDS_MAX=3
DNSK_CGPIDS_CURRENT_SHELL=1
cgroup: fork rejected by pids controller in /t/a
DNSK_CGPIDS_SPAWNED=1
DNSK_CGPIDS_CURRENT=2
DNSK_CGPIDS_PEAK=3
DNSK_CGPIDS_EVENTS=max 1
DNSK_CGPIDS_A_AFTER_MIGRATE=2
DNSK_CGPIDS_B_AFTER_MIGRATE=1
DNSK_CGPIDS_OK
DNSK_QEMU_OK
```

观察点：root 显示 pids；`t/a` 继承 pids 文件；limit 3 时 shell+1
sleep 后再 fork 被拒；peak=3；events 计 1；迁移一个 sleep 到 `t/b`
后 `a=2, b=1`。

## Virtual 结果

待 `oracle-nopids` 镜像构建后运行同一脚本并逐项对比。任何一项不同都
阻塞 cgpids 状态更新，不把“文件存在”当作通过。

## Device Image (beryl) 进展

- `device-image.yml` 直接从 beryl 线刷包提取 `boot.img`，`unpack_bootimg`
  得到 raw Image；QEMU `-M virt` 启动成功（run 42222245693）：
  `Linux version 6.12.30-android16-5-g1ed949324a3e-ab13881345-4k`，
  PL011 console，`DNSK_DEVICE_KERNEL_OK` / `DNSK_QEMU_OK`。
- 设备 `module_layout` CRC 从 vmlinux `__kcrctab` 解析为 `0xe976b219`；
  device-image 工作流同时 patch vermagic 与该 CRC。
- 早期 `finit_module: Exec format error` 的根因是 loader 传入
  `MODULE_INIT_IGNORE_MODVERSIONS`：该 flag 把 `versindex` 置 0，触发
  `check_modstruct_version -> try_to_force_load`，而设备未开
  `CONFIG_MODULE_FORCE_LOAD`。改为 flags=0、只 patch `module_layout` 后，
  `dnsk_core`/`dnsk_cgpids` 在设备 Image 上加载成功。
- 设备 Image 上 mkdir hook 已命中；`cgroup_addrm_files` 返回 0 但新文件不可见。
  根因是 cgroup kernfs root 使用 `KERNFS_ROOT_CREATE_DEACTIVATED`，mkdir 返回后
  新建的 cftype 文件默认未激活；修复为 add 后调用 `kernfs_activate(child->kn)`。
  待复测确认 pids.max/current 等文件可见并完成差分。

## Device Image virtual 结果 (run 42228719092)

同一 `tests/qemu/init-cgpids` 在 beryl 设备 Image 上加载
`dnsk_core.ko` + `dnsk_cgpids.ko`（xt104 profile，CI artifact 42227938979），
vermagic/`module_layout` 由 device-image 工作流 patch：

```text
DNSK_CGPIDS_LKM_OK
DNSK_CGPIDS_CONTROLLERS=cpuset cpu io memory pids
DNSK_CGPIDS_ROOT_SUBTREE=pids
DNSK_CGPIDS_MAX_BEFORE=max
DNSK_CGPIDS_MAX=3
DNSK_CGPIDS_CURRENT_SHELL=1
cgroup: fork rejected by dnsk pids controller
DNSK_CGPIDS_SPAWNED=1
DNSK_CGPIDS_CURRENT=2
DNSK_CGPIDS_PEAK=3
DNSK_CGPIDS_EVENTS=max 1
DNSK_CGPIDS_A_AFTER_MIGRATE=2
DNSK_CGPIDS_B_AFTER_MIGRATE=1
DNSK_CGPIDS_OK
DNSK_QEMU_OK
```

与 Native baseline（run 42216318718）逐项一致，包括 fork EAGAIN、peak、
events 和迁移计数。该结果来自设备当前 6.12.30 内核 Image，而不是 DDK。
Docker 29.x `--pids-limit` 与真机 `ksud insmod` 仍是后续门控。
