# dnsk_ns (M1 development)

Vendored Droidspaces-LKM 6.12 PID/IPC namespace implementation.

- Build target: `android16-6.12` only for now.
- Loader: `dnskctl load ns` passes `gki=1 activate=1`.
- Status: development; no release/tag claim.
- Core status bridge is optional: with core loaded first, `/proc/dnsk/status` shows the
  `ns` feature row; without core the namespace implementation still works.
- `activate=0` loads and unloads cleanly; `tests/device/core-bridge-smoke.sh` verifies
  the bridge and the core pin/unpin order.
- Upstream pins the module after activation; `rmmod` is not expected to work
  after activation. Reboot is required to replace it.
