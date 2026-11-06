Test-only runtime assets copied read-only from the beryl device:

- `ksud`: ReSukiSU 4.2.0-rc3 (`com.resukisu.resukisu`), GPL-3.0; contains
  the bundled `kernelsu.ko` used by `ksud late-load`.
- `android/system/{bin/linker64,lib64/*}`: Android 16 bionic runtime
  (Apache-2.0), needed because ksud is dynamically linked to
  `/system/bin/linker64`.

These files are used only in the QEMU device-image harness, not shipped in
the KernelSU module package.
