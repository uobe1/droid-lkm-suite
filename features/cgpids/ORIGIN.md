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
not native. See `docs/feasibility/cgpids.md` for the design and the list of
unsupported v1/threaded/id-enumeration semantics.

SPDX-License-Identifier: GPL-2.0-only
