# QEMU test inputs

QEMU tests are evidence, not a formality. The first real harness is
`.github/workflows/qemu-core.yml` (manual `workflow_dispatch`):

1. The DDK job builds core/ns/devtmpfs/netfilter against
   `/opt/ddk/kdir/android16-6.12` and creates a boot `Image` from the DDK
   `vmlinux` with objcopy.
2. The boot job extracts a pinned Alpine 3.20.3 arm64 minirootfs, adds the
   modules and `tests/qemu/init`, and boots `qemu-system-aarch64 -M virt`
   under TCG.
3. The stock-GKI init script loads core, checks `patch=ready`/`panic=ready`,
   unloads it, and prints `DNSK_QEMU_OK`; the job fails on panic/Oops.

Feature modules (`ns`, `devtmpfs`, `netfilter`, `ipset`) import non-exported
kernel internals that the KernelSU loader resolves on the phone. Plain stock
`insmod` rejects them, so QEMU currently gates core and the runtime-symbol
path; feature activation is exercised on the KSU device (and needs a
KSU-enabled VM or an exports-augmented test kernel to run in the cloud).

Planned matrix:

- **feature-off kernel**: SYSVIPC/PID_NS/IPC_NS/DEVTMPFS/USER_NS/NF_TABLES/
  IP_SET/CGROUP_PIDS/TMPFS_XATTR/POSIX_ACL disabled; core load/unload and
  feature emulation tests.
- **AOSP Native oracle**: the same source with the relevant configs enabled;
  used for TMPFS/USER_NS/CGROUP_PIDS differential tests.
- **active feature tests**: ns/devtmpfs/netfilter activation in disposable VMs,
  then container workloads (Docker 29.x, droidspaces, UFW, Fail2ban) on pinned
  arm64 rootfs images.

A QEMU run counts only when its log and job URL are attached to the release
evidence. Do not mark a feature validated from a build alone.

## DDK image facts

The `android16-6.12-20260828` DDK kdir ships `vmlinux`, `System.map`, `.config`
and `Module.symvers` for Linux **6.12.76-4k**; `objcopy` of that `vmlinux`
boots under `-M virt` and matches the modules built in the same container.
Its config has `PID_NS`, `IPC_NS`, `SYSVIPC`, `POSIX_MQUEUE`, `DEVTMPFS`,
`NF_TABLES`, `IP_SET` and `USER_NS` disabled, which is the feature-off kernel
these tests need. `9P_FS=n`, so modules must be embedded in the initramfs;
`MODULE_SIG=y` with `MODULE_SIG_FORCE` off, so unsigned modules load with a
taint.

Google's public GKI `Image` for build 13881345 is the device's exact
`6.12.30-android16-...` kernel, but its version and symbol CRCs differ from the
DDK 6.12.76 build; it is not used to load DDK-built modules. It remains a
candidate for Native-oracle work with matching builds.
