# dnsk_diag (diagnostic helper)

Read-only diagnostic module for an active `dnsk_ns`. It never installs hooks and
never changes namespace or mqueue state; it only reads kernel structures through
`copy_from_kernel_nofault()` and prints `dnsk_diag:` lines to the kernel log.

## Scope

- `android16-6.12` only.
- Builds as an independent `dnsk_diag.ko` (`features/diag/Kbuild`), wired into
  `ci-fast.yml` and `qemu-core.yml`.
- Dumps the IPC namespace's `mq_mnt`/superblock counters and related state for
  debugging the `ns` feature.

## Status

Diagnostic helper, not a feature module. No O/V classification. No device gate
is recorded for it; it is built and loaded only by the QEMU diagnostic path.

## Design

`dnsk_diag.c` resolves the `ipc_namespace` offsets from the loaded `dnsk_ns`
state, reads each pointer with `copy_from_kernel_nofault()`, and logs the
resolved mount/superblock counts. It deliberately avoids module-internal
symbols that are not exported.

## Non-goals

- Not a user-facing feature; it does not provide namespace capabilities.
- Not a replacement for `dnskctl doctor` or pstore forensics.
- Not part of the device load sequence for users.

## Evidence

Build coverage: `ci-fast.yml` and `qemu-core.yml` module lists (2026-11-27).
No functional gate evidence; treat any output as diagnostic only.
