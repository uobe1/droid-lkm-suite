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

## Native slot injection and the disabled `rw_reloc` probe

`native.c` also has an O-side native path: it installs a real `cgroup_subsys`
for pids into the spare `net_prio` slot 7 and lets the kernel's own cgroup core
drive it. That is the shipped path; `native=0` keeps the virtual controller
above.

`native.c` additionally carries a **default-off mechanism prototype**,
`dnsk_cgpids.rw_reloc=1`. It copies `cgroup_subsys[]` to a one-entry-bigger,
same-content array and retargets every `ADRP+ADD` / `ADRP+LDR` reference pair
that pointed at the old array. Because the copy has identical contents, a missed
reader still sees the whole original table, so it is a *safe mechanism probe*,
**not** a working 9th subsystem:

- it does **not** rewrite the `CGROUP_SUBSYS_COUNT` immediate the compiler
  folded into loop bounds, masks and `cmp` operands. The count is an enum
  constant with no symbol, so there is no authoritative site list; the rewrite is
  deferred on purpose (reason and a runtime detection method are in the in-code
  comment, and in `docs/notes/On-cgpids-reloc-2026-11-21.md`, local material);
- it does **not** touch the shadow arrays sized by `CGROUP_SUBSYS_COUNT`
  (`cgroup_subsys_name[]`, `*_enabled_key[]`, `*_on_dfl_key[]`, and the embedded
  `css_set->subsys[]` / `cgroup->subsys[]` / `e_csets[]` / `nr_dying_subsys[]`).

The decode/encode helpers and the reference-pair decision are pure functions in
a kernel-free block of `native.c`, covered by `tests/host/test_aarch64_refs.py`
(the host test extracts that exact C text, compiles it and drives it through
ctypes). `rw_reloc` has **not** been built into a kernel module or run on a
device; that needs one Action gate.
