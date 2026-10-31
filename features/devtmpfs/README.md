# dnsk_devtmpfs (M1 development)

Late-loaded devtmpfs for android16-6.12.

- Build: `make -C features/devtmpfs KDIR=... modules`.
- Loader parameters: `gki=1 activate=1` (active) or `activate=0` (no-op load).
- Internal mount only; it does not replace or mount over the host `/dev`.
- Activation pins the module; replacing it requires a reboot.
- Status: development; no release/tag claim.
