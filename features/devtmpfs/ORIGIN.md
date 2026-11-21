# dnsk_devtmpfs origin

Upstream: <https://github.com/Shikochin/Droidspaces-LKM>
Commit: `8c14b35ce353bf1ddab56150733854e3eb440d68` (v0.1.5)
License: GPL-2.0-only (see `vendor/COPYING`; vendored source keeps its SPDX).

Vendored paths:

- `vendor/devtmpfs/*` from `src/devtmpfs/*`
- `vendor/ksud/*`: local shim, not upstream.  `kallsyms_lookup_name` is a
  plain import resolved by `ksud insmod`; the upstream kallrecon scanner is
  not vendored.

Local changes:

- `Kbuild`/`Makefile` are project-specific; output is `dnsk_devtmpfs.ko`.
- `vendor/devtmpfs/main.c`: the PLZ110/VM exact-target gate is replaced with a
  `gki=1` + `6.12.*-android16-*` gate. `activate=0` loads the module as a no-op;
  `activate=1` is required to create the internal devtmpfs mount and install
  device lifecycle hooks. A no-op `module_exit` was added so the inactive mode
  can be unloaded. The active mode does not self-pin; the exit restores hooks
  and unregisters the type.
- `trigger=entry|bus` (default `entry`) is a local opt-in. `entry` keeps the
  original `device_add`/`device_del` entry hooks. `bus` installs one
  `bus_notify` hook so the node is created before the bus notifier chain/uevent
  and deleted after the notifier chain. `trigger_plan.h` carries the pure
  ordering decision; `tests/host/test_devtmpfs_trigger_order.py` compiles that
  real C block and checks the wrapper call order. Neither mode is claimed as
  Native before the Action differential.

Current stage: M1 development, android16-6.12 only. Not release-supported.
