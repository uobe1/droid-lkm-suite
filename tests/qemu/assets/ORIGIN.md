Test-only runtime assets copied read-only from the beryl device:

- `ksud`: ReSukiSU 4.2.0-rc3 (`com.resukisu.resukisu`), GPL-3.0; contains
  the bundled `kernelsu.ko` used by `ksud late-load`.
- `android/system/{bin/linker64,lib64/*}`: Android 16 bionic runtime
  (Apache-2.0), needed because ksud is dynamically linked to
  `/system/bin/linker64`.
- `selinux-precompiled-sepolicy.gz`: beryl
  `/vendor/etc/selinux/precompiled_sepolicy`, 1599207 bytes, sha256
  `f5ce4876cbd711597c5acbc6ace4afc730ade0e83d1194df320b6bd98237d755`
  (gzip -9).  KSU late-load copies the loaded policy and faults on NULL when
  the harness boots without one, so the VM loads this before `ksud late-load`.

These files are used only in the QEMU device-image harness, not shipped in
the KernelSU module package.
