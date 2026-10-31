# dnsk_devtmpfs origin

Upstream: <https://github.com/Shikochin/Droidspaces-LKM>
Commit: `8c14b35ce353bf1ddab56150733854e3eb440d68` (v0.1.5)
License: GPL-2.0-only (see `vendor/COPYING`; vendored source keeps its SPDX).

Vendored paths:

- `vendor/devtmpfs/*` from `src/devtmpfs/*`
- `vendor/kallrecon/*` from `vendor/kallrecon/*`

Local changes:

- `Kbuild`/`Makefile` are project-specific; output is `dnsk_devtmpfs.ko`.
- `vendor/devtmpfs/main.c`: the PLZ110/VM exact-target gate is replaced with a
  `gki=1` + `6.12.*-android16-*` gate. `activate=0` loads the module as a no-op;
  `activate=1` is required to create the internal devtmpfs mount and install
  device lifecycle hooks. A no-op `module_exit` was added so the inactive mode
  can be unloaded; the active mode is still pinned by `__module_get()`.

Current stage: M1 development, android16-6.12 only. Not release-supported.
