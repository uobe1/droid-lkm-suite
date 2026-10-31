# QEMU test inputs

QEMU tests are evidence, not a formality. The first real harness is
`.github/workflows/qemu-core.yml` (manual `workflow_dispatch`):

1. The DDK job builds core/ns/devtmpfs/netfilter against
   `/opt/ddk/kdir/android16-6.12` and creates a boot `Image` from the DDK
   `vmlinux` with objcopy.
2. The boot job extracts a pinned Alpine 3.20.3 arm64 minirootfs, adds the
   modules and `tests/qemu/init`, and boots `qemu-system-aarch64 -M virt`
   under TCG.
3. The init script loads core + ns with `activate=0`, checks the status bridge,
   unloads both, and prints `DNSK_QEMU_OK`; the job fails on panic/Oops.

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
