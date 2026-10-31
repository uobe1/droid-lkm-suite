# QEMU test inputs

QEMU tests are evidence, not a formality. A run needs:

1. An arm64 kernel `Image` built from the same KMI source as the module.
2. A rootfs/initramfs with `insmod`/`rmmod` and a shell.
3. The module built against the same KDIR.

The planned matrix:

- **feature-off kernel**: SYSVIPC/PID_NS/IPC_NS/DEVTMPFS/USER_NS/NF_TABLES/
  IP_SET/CGROUP_PIDS/TMPFS_XATTR/POSIX_ACL disabled; core load/unload and
  feature emulation tests.
- **AOSP Native oracle**: the same source with the relevant configs enabled;
  used for TMPFS/USER_NS/CGROUP_PIDS differential tests.

`tests/qemu/run-qemu.sh` is a real runner for a prepared kernel/initramfs. It
fails closed when an input is missing; it does not synthesize a pass.

No QEMU result exists yet for this repository. Do not mark any feature
validated until the logs are attached to a release.
