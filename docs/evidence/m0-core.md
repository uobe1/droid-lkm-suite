# M0 core evidence

日期：2026-10-31。范围仅 `dnsk_core.ko`；没有任何 feature 模块被声明为可用。

## CI

- Build/package：GitHub Actions run `41232605263`，结论 success。
  - 8 个 KMI 的 core 全部构建成功。
  - `package-dev-612` 成功生成 dev KSU ZIP。
- Docs：run `41232605351`，结论 success；Pages 已部署到 <https://uobe1.github.io/droid-lkm-suite/>。

Dev artifact：

```text
droid_lkm_suite-v1.0.0dev0-android16-6.12.zip
sha256 976e66745bdfa538b8c528f054fec089ca48ae103f0ad199dc7295cbf5096f86
```

## 真机加载

设备：`termux.native`，内核 `6.12.30-android16-5-g1ed949324a3e-ab13881345-4k`，KMI `android16-6.12`。

加载命令：

```sh
su -c '/data/adb/ksud insmod /data/data/com.termux/files/home/dnsk-dev/kmi/android16-6.12/dnsk_core.ko'
```

结果：

```text
Loaded kernel module: .../dnsk_core.ko
dnsk_core 45056 0

dnsk_core abi=1
kmi=6.12.30-android16-5-g1ed949324a3e-ab13881345-4k
patch=ready
panic=ready
hooks=0
features=0
```

dmesg：`dnsk: core ready abi=1 patch=ready`。没有 Oops/panic。

## 真机卸载

```sh
su -c '/data/adb/ksu/bin/busybox rmmod dnsk_core'
```

结果：`rmmod_rc=0`；`lsmod` 不再有 `dnsk_core`；`/proc/dnsk/status` 返回 `No such file or directory`。没有 Oops/panic。

## 结论与边界

- 已证明：运行时 kallsyms resolver、patch 后端初始化、`/proc/dnsk/status`、panic notifier、模块加载/卸载在此设备可用。
- 未证明：任何 feature、Native 差分、QEMU 测试、Docker/NixOS/UFW/Fail2ban、USER_NS/TMPFS/CGROUP_PIDS。
- 因此当前仍为 M0 core scaffold；不得据此宣称任何容器子系统已支持。
