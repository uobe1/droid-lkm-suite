# dnsk_ns origin

Upstream: <https://github.com/Shikochin/Droidspaces-LKM>
Commit: `8c14b35ce353bf1ddab56150733854e3eb440d68` (v0.1.5)
License: GPL-2.0-only (see `vendor/COPYING`; vendored source keeps its SPDX).

Vendored paths:

- `vendor/port/*` from `src/port/*` (android16-6.12 PID/IPC port)
- `vendor/gki-target.h` from `src/gki-target.h`
- `vendor/kallrecon/*` from `vendor/kallrecon/*`

Local changes:

- `Kbuild` and `Makefile` are project-specific; upstream build files are not vendored.
- Module output name is `dnsk_ns.ko`; upstream internal `ds_` prefixes are retained.
- Local edits to vendored `main.c` (when made) are listed here with a diff summary.

Current stage: M1 development. The module is built only for `android16-6.12`
until the other port directories are vendored and gated. It is not a release
artifact and must not be marked supported before device/QEMU evidence.
