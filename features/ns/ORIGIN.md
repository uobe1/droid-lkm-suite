# dnsk_ns origin

Upstream: <https://github.com/Shikochin/Droidspaces-LKM>
Commit: `8c14b35ce353bf1ddab56150733854e3eb440d68` (v0.1.5)
License: GPL-2.0-only (see `vendor/COPYING`; vendored source keeps its SPDX).

Vendored paths:

- `vendor/port/*` from `src/port/*` (android16-6.12 PID/IPC port)
- `vendor/gki-target.h` from `src/gki-target.h`
- `vendor/ksud/*`: local shim, not upstream.  `kallsyms_lookup_name` and
  `module_kallsyms_lookup_name` are plain imports resolved by `ksud insmod`;
  the upstream kallrecon kallsyms scanner is not vendored.

Local changes:

- `Kbuild` and `Makefile` are project-specific; upstream build files are not vendored.
- Module output name is `dnsk_ns.ko`; upstream internal `ds_` prefixes are retained.
- `vendor/port/exit.c`/`port.h`/`main.c`: `proc_exit_connector()` is an optional
  `CONFIG_CONNECTOR` facility and is absent on the target 6.12.30 kernel. The
  call is now a runtime-resolved function pointer; when the symbol is absent,
  the optional proc-connector exit event is not emitted (matching a kernel
  built without CONFIG_CONNECTOR). No PID/IPC semantics depend on it.
- `vendor/port/hooks.c`: accept the target kernel's `0xf800865e` SCS push in
  the movable prologue, same as upstream devtmpfs; it contains no PC operand.
- `vendor/port/main.c`: hook preparation is skipped when `activate=0`; only
  activation requires the entry preflight. The optional `core_bridge.c` adds a
  development status row through `dnsk_core_get` resolved at runtime; a missing
  core only disables the row.

Current stage: M1 development. The module is built only for `android16-6.12`
until the other port directories are vendored and gated. It is not a release
artifact and must not be marked supported before device/QEMU evidence.
