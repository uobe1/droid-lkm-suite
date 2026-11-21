# ORIGIN

The accounting and file semantics follow upstream Linux 6.12
`kernel/cgroup/pids.c`:

- hierarchical charge/uncharge, `pids.max` sentinel `PID_MAX_LIMIT + 1`,
  `-EAGAIN` on limit, event counters, peak watermark;
- fork enforcement uses the cgroup core `can_fork`/`cancel_fork` callback
  slots, and release uses the `release` slot, exactly where the native pids
  controller runs;
- migration accounting observes `cgroup_migrate_execute` and mirrors the
  native `can_attach`/`cancel_attach` moves.

The module does not copy the native controller as a `cgroup_subsys`; it cannot
register one at runtime. The virtual controller is documented as `VIRTUAL`,
not native. See `docs/evidence/feasibility-cgpids.md` (local material, not shipped) for the design and the list of
unsupported v1/threaded/id-enumeration semantics.

## Why relocating `cgroup_subsys[]` is not the same as growing it

Upstream sizes several structures and loop bounds by the compile-time enum
`CGROUP_SUBSYS_COUNT` (`include/linux/cgroup-defs.h:44-46`). The same constant
sizes `cgroup_subsys[]` / `cgroup_subsys_name[]` and the `*_enabled_key[]` /
`*_on_dfl_key[]` pointer arrays (`kernel/cgroup/cgroup.c:158-192`), and the
embedded arrays `css_set->subsys[]` / `e_cset_node[]` (`cgroup-defs.h:240,280`)
and `cgroup->subsys[]` / `nr_dying_subsys[]` / `e_csets[]`
(`cgroup-defs.h:524,530,547`). Iteration uses the same constant: `for_each_subsys`
bounds on `CGROUP_SUBSYS_COUNT` (`kernel/cgroup/cgroup-internal.h:182-184`), and
`/proc/cgroups` is produced by that loop (`kernel/cgroup/cgroup-v1.c:676-694`).
Moving the pointer array alone therefore leaves every folded bound reading the
old value, which is why the `rw_reloc` probe is a mechanism probe, not a working
extension. (Paths are AOSP 6.12.30, the same KMI as the device Image.)

SPDX-License-Identifier: GPL-2.0-only
