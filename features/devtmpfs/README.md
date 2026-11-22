# dnsk_devtmpfs (R / development)

Late-loaded devtmpfs for android16-6.12, delivered as route E: the module
supplies the implementation and the kernel VFS executes it, instead of a
userspace or display-only bypass.

- Build: `make -C features/devtmpfs KDIR=... modules`.
- Loader parameters: `gki=1 activate=1` (active) or `activate=0` (no-op load);
  optional `trigger=entry|bus` (default `entry`).
- Internal mount only; it does not replace or mount over the host `/dev`.
- Activation does not self-pin; `module_exit` restores the hooks and
  unregisters the type. Existing devtmpfs mounts keep their superblock but stop
  receiving updates.
- Status: R / development. The default `trigger=entry` path shows zero
  differences in the whole-suite semantic differential, but `trigger=bus`
  ordering and gaps #2-#6 are still unverified, so no Native (N) claim and no
  release/tag claim.

## Runtime prerequisite and route E

The device kernel has `CONFIG_DEVTMPFS` compiled out: the beryl config
(`profiles/device/beryl-6.12.30.config.gz`) has `# CONFIG_DEVTMPFS is not set`
(decompressed line 1836) while `CONFIG_TMPFS=y` (line 7101); the AOSP
gki_defconfig has no `CONFIG_DEVTMPFS` entry at all. The kernel therefore has no
devtmpfs code, no devtmpfs `file_system_type`, no `kdevtmpfs` thread, and no
static data or call site for the RW primitive to rewrite.

Native semantics are still reachable by route E. The module supplies a real
`file_system_type`, mounts it with `vfs_kern_mount()`, registers it with
`register_filesystem()`, and runs the devtmpfs worker as a kthread. The kernel
VFS then creates and deletes real dentries and inodes on a real superblock;
`/proc/filesystems` lists the type, and `mount(2)` goes through
`public_dev_mount()` to share the same superblock. That is "the kernel executes
the semantics", not a module-side imitation.

Known residual differences against a kernel built with `CONFIG_DEVTMPFS=y`:

1. Device add/delete trigger ordering: the native call sites in
   `device_add()`/`device_del()` are compiled out, so the module triggers node
   updates from hooks. The default `trigger=entry` keeps the old ordering
   (node after uevent/driver probe, gone before `bus_notify(DEL)`); the opt-in
   `trigger=bus` moves the add node before the notifier chain and the del node
   after it. Gap #1 is implemented but **not verified**: the bus gate run
   `44975808897` failed during load with 57 `dnsk_devtmpfs: Unknown symbol`
   lines, i.e. the loader did not resolve the module's imports, so the trigger
   assertions never ran. That is a loader-side failure, not a module verdict.
2. The worker runs with the loader's credentials instead of the kernel's
   credentials, so SELinux/AVC decisions can differ.
3. No automatic `/dev` mount and no `devtmpfs.mount=` handling. The device
   config has no `CONFIG_DEVTMPFS_MOUNT`, so the default `mount_dev=0` matches,
   but a cmdline override is not implemented.
4. The worker sets a private fs root with `set_fs_root()` instead of creating a
   private mount namespace and chroot, which is observable in
   `/proc/<pid>/mountinfo`.
5. Thread name, log prefix and extra module parameters differ.
6. After unload, existing mounts keep the old superblock and stop receiving
   updates; a reload creates a new instance.

The dual `file_system_type` pair and the 0755 root mode match native.

## Current state (2026-11-22)

- Route E is in place: the module supplies a real `file_system_type`, mounts it
  with `vfs_kern_mount()`, registers it with `register_filesystem()`, and the
  kernel VFS creates/deletes real dentries and inodes.
- Whole-suite semantic differential (run `44975808897`): every `DEVTMPFS_*` key
  matched the native oracle. That covers the default `trigger=entry` path on
  the measured surface only.
- Gap #1 (`trigger=bus` ordering) is unverified: the dedicated bus job
  (`44975808897`) failed at module load with 57 `Unknown symbol` lines, before
  any trigger assertion. Fix the loader invocation (`ksud late-load` plus
  `insmod`) and rerun before writing any closure.
- Gaps #2-#6 remain: worker credentials, automatic `/dev` mount and
  `devtmpfs.mount=`, kthread namespace/root, thread name/log/parameters, and
  unload/reload instance behaviour.
- O is the mechanism classification (kernel VFS executes the semantics); R is
  the verification status. Do not read the zero-difference differential as a
  Native declaration.

## trigger=entry|bus

`trigger=entry` (default) keeps the existing `device_add`/`device_del` entry
hooks and their output unchanged. `trigger=bus` installs one `bus_notify` hook
instead:

- `BUS_NOTIFY_ADD_DEVICE`: create the node before the notifier chain, so it is
  already present when uevent and `bus_probe_device()` run.
- `BUS_NOTIFY_DEL_DEVICE`: run the notifier chain first (notifiers still see
  the node), then remove the node.

`trigger_active` is 1 only after the bus hook is installed. Bus mode is
observable through these lines (entry mode emits none of them):

```text
droidspaces_devtmpfs: trigger=bus active
droidspaces_devtmpfs: bus trigger: add <dev> before-notify
droidspaces_devtmpfs: bus trigger: del <dev> after-notify
```

If an Action shows the bus path is wrong, unload the module and load it again
without the parameter (default `entry`); no persistent state changes.

One Action can verify: shared superblock (`st_dev` of two mounts), node
type/major/minor/mode/uid/gid against an oracle kernel, loop add/remove node
lifecycle and bus-notifier ordering, SELinux/AVC behaviour under enforcing,
worker name/mountinfo/namespace, and unload/reload behaviour.

For gap #1 specifically, the probe should emit
`DNSK_TRIGGER_BUS_READY` after reading `trigger_active=1`, then on
`LOOP_CTL_ADD` record `DNSK_TRIGGER_BUS_ADD_NODE_PRESENT` if the mounted
devtmpfs already has `loopN` when the uevent arrives, and on `LOOP_CTL_REMOVE`
record `DNSK_TRIGGER_BUS_DEL_NODE_PRESENT` from a bus-notifier probe that still
sees the node. Loop devices have no bus, so use a tiny probe module that
registers `bus_register_notifier(&platform_bus_type, ...)` and adds/removes a
platform device with `dev.devt` set; its `BUS_NOTIFY_DEL_DEVICE` callback stats
the mounted devtmpfs node. A control run with `trigger=entry` must show
`DNSK_TRIGGER_ENTRY_ADD_NO_NODE` (or a lost race); `DNSK_TRIGGER_ROLLBACK_OK`
after reloading without the parameter confirms the fallback.

The module refuses to shadow a kernel that ships devtmpfs: at init it calls
`get_fs_type("devtmpfs")` and returns `-EEXIST` if the type exists.

Local material, not part of this page: `docs/notes/On-devtmpfs-native-2026-11-21.md`
holds the verdict, the gap table and the AOSP `file:line` evidence.
