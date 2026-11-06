# dnsk_cgpids

cgroup v2 virtual pids controller. Native `CONFIG_CGROUP_PIDS` is compiled out
on the target; a bounded LKM cannot extend `cgroup_subsys[]`, `css_set[]` or
the subsystem enum. This module provides the observable controller instead:

- Real `pids.max`, `pids.current`, `pids.peak`, `pids.events`,
  `pids.events.local` files, added on cgroup mkdir.
- Hierarchical counters charged at fork and uncharged at task release.
- Real `-EAGAIN` when a fork would exceed any ancestor limit.
- Migration accounting through a `cgroup_migrate_execute` kretprobe, with
  rollback when migration fails.
- `cgroup.controllers` / `cgroup.subtree_control` expose and accept `pids`
  without enabling a fake native controller.

Scope and limits:

- cgroup v2 only. cgroup v1 `--pids-limit` remains unsupported.
- Not a native `cgroup_subsys`; `/proc/cgroups` and controller IDs do not list
  pids. This is reported as `VIRTUAL`, never as native.
- Files are added to cgroups created after module load; pre-existing cgroups
  are not retrofitted.
- A cgroup state is created on demand and pins the module until reboot
  (matching the plan's "cgpids fixed after activation" rule); unload reports
  "needs reboot" once any state has been created.

Acceptance is Docker 29.x `--pids-limit` plus a Native-vs-Virtual differential
contract against an AOSP/DDK oracle with `CONFIG_CGROUP_PIDS=y`.
