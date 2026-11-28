# dnsk_cgpids

pids cgroup 控制器, 有两条路径: 默认的 native 槽位注入 (`native.c`) 与
`native=0` 时才使用的虚拟控制器 (`virtual.c`).

## Native 槽位注入 (默认)

`native.c` installs a real `cgroup_subsys` for pids into the spare `net_prio`
slot 7 and lets the kernel's own cgroup core drive it; `/proc/cgroups` lists
`pids`. The kernel's `cgroup_subsys` has no `owner` field on this KMI, so the
module holds a reference to itself while the slot is installed. `dnskctl`
unload first calls `dnsk_cgpids_native_quiesce()`, which hands the slot back
and drops that reference; only then can `rmmod` reach `module_exit`. A bare
`rmmod` without quiesce returns `EAGAIN`, and quiesce itself returns `-EBUSY`
while tasks are still accounted to the slot.

## 虚拟控制器 (`native=0`)

Native `CONFIG_CGROUP_PIDS` is compiled out on the target; the virtual path
provides the observable controller instead:

- Real `pids.max`, `pids.current`, `pids.peak`, `pids.events`,
  `pids.events.local` files, added on cgroup mkdir.
- Hierarchical counters charged at fork and uncharged at task release.
- Real `-EAGAIN` when a fork would exceed any ancestor limit.
- Migration accounting through an inline hook on `cgroup_migrate_execute`,
  with rollback when migration fails.
- `cgroup.controllers` / `cgroup.subtree_control` expose and accept `pids`
  without enabling a fake native controller.

Scope and limits:

- cgroup v2 only. cgroup v1 `--pids-limit` remains unsupported.
- The virtual path is not a native `cgroup_subsys`; it is reported as
  `VIRTUAL`, never as native. The default native path does register a real
  subsystem and does show up in `/proc/cgroups`.
- Virtual-path files are added to cgroups created after module load;
  pre-existing cgroups are not retrofitted.
- Virtual per-cgroup state is freed with `kfree_rcu()`, so no state outlives
  the module and the virtual path does not pin it.

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
