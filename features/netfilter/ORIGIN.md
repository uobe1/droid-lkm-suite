# dnsk_netfilter origin

Upstream: <https://github.com/Shikochin/Droidspaces-LKM>
Commit: `8c14b35ce353bf1ddab56150733854e3eb440d68` (v0.1.5)
License: GPL-2.0-only (see `vendor/COPYING`; vendored source keeps its SPDX).

Vendored paths: `vendor/netfilter/*` from `src/netfilter/*`.

Local changes:

- `Kbuild`/`Makefile` are project-specific; output is `dnsk_netfilter.ko`.
- `vendor/netfilter/main.c`: the PLZ110/VM exact-target gate is replaced with a
  `gki=1` + `6.12.*-android16-*` gate. `activate=0` loads as a no-op;
  `activate=1` registers the nftables/xt_addrtype units.
- Build target is android16-6.12 only for now. Not release-supported.
