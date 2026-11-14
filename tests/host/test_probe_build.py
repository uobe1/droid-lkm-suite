"""Compile the QEMU userland probes against the NDK/bionic headers."""

import pathlib
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SYSROOT = pathlib.Path("/usr/local/share/ndk/sysroot")
# bionic has no POSIX mqueue.h; the workflow builds these with glibc instead.
BIONIC_GAPS = {"ipcns_leak.c", "ipc_probe.c"}


@unittest.skipUnless(shutil.which("clang") and SYSROOT.is_dir(),
                     "clang or the NDK sysroot is not available")
class ProbeSyntaxTest(unittest.TestCase):
    def test_probes_compile_against_ndk_headers(self):
        probes = sorted((ROOT / "tests" / "qemu").glob("*.c"))
        self.assertTrue(probes)
        failed = []
        for probe in probes:
            if probe.name in BIONIC_GAPS:
                continue
            result = subprocess.run(
                ["clang", "--target=aarch64-linux-android21",
                 "--sysroot=" + str(SYSROOT), "-fsyntax-only", "-Wall",
                 "-Wextra", str(probe)],
                capture_output=True, text=True)
            if result.returncode:
                failed.append((probe.name, result.stderr.splitlines()[:2]))
        self.assertEqual(failed, [])


if __name__ == "__main__":
    unittest.main()