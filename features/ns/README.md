# dnsk_ns (M1 development)

Vendored Droidspaces-LKM 6.12 PID/IPC namespace implementation.

- Build target: `android16-6.12` only for now.
- Loader: `dnskctl load ns` passes `gki=1 activate=1`.
- Status: development; no release/tag claim.
- Requires core loaded first (once the core bridge is added).
- Upstream pins the module after activation; `rmmod` is not expected to work
  after activation. Reboot is required to replace it.
