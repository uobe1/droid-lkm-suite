# SPDX-License-Identifier: GPL-3.0-only
"""Source-level guards for the devtmpfs E-route decision and wiring.

Scope: these tests pin *tracked-file invariants* only. They verify
  (a) the decision premise -- the device kernel has no CONFIG_DEVTMPFS;
  (b) the E-route wiring -- the vendored source mounts/registers a real
      file_system_type and runs the devtmpfs worker through the kernel;
  (c) the refusal to shadow a kernel that already ships devtmpfs;
  (d) the documented R/development status and residual-gap list.

They do NOT execute kernel code and cannot prove runtime equivalence with a
CONFIG_DEVTMPFS=y kernel. That needs the QEMU-oracle/device Action listed in
features/devtmpfs/README.md. They never read docs/notes or docs/evidence,
which are gitignored and absent in a fresh clone.
"""

import gzip
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEVICE_CONFIG = ROOT / "profiles" / "device" / "beryl-6.12.30.config.gz"
MAIN_C = ROOT / "features" / "devtmpfs" / "vendor" / "devtmpfs" / "main.c"
DEVTMPFS_C = ROOT / "features" / "devtmpfs" / "vendor" / "devtmpfs" / "devtmpfs.c"
HOOKS_C = ROOT / "features" / "devtmpfs" / "vendor" / "devtmpfs" / "hooks.c"
STATUS_MD = ROOT / "docs" / "status.md"
README = ROOT / "features" / "devtmpfs" / "README.md"

# Line numbers cited by the status matrix and features/devtmpfs/README.md.
# Keeping them asserted keeps the citations honest.
CONFIG_LINE = 1836
TMPFS_LINE = 7101


class DevtmpfsERouteGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with gzip.open(DEVICE_CONFIG, "rt", encoding="utf-8") as f:
            cls.config_lines = [line.rstrip("\n") for line in f]
        cls.config_text = "\n".join(cls.config_lines)
        cls.main_c = MAIN_C.read_text(encoding="utf-8")
        cls.devtmpfs_c = DEVTMPFS_C.read_text(encoding="utf-8")
        cls.hooks_c = HOOKS_C.read_text(encoding="utf-8")

    def test_device_config_has_devtmpfs_disabled(self):
        self.assertIn("# CONFIG_DEVTMPFS is not set", self.config_text)
        self.assertNotIn("CONFIG_DEVTMPFS=y", self.config_text)
        self.assertEqual(
            self.config_lines[CONFIG_LINE - 1],
            "# CONFIG_DEVTMPFS is not set",
            "CONFIG_DEVTMPFS line moved; docs cite line 1836",
        )

    def test_device_config_keeps_tmpfs(self):
        self.assertIn("CONFIG_TMPFS=y", self.config_text)
        self.assertEqual(
            self.config_lines[TMPFS_LINE - 1],
            "CONFIG_TMPFS=y",
            "CONFIG_TMPFS line moved; docs cite line 7101",
        )

    def test_e_route_is_wired_through_kernel_vfs(self):
        # Route E = real fs type + kernel mount/registration + kernel worker.
        for needle in (
            "vfs_kern_mount(",
            "register_filesystem(dev_fs_type)",
            'kthread_create(devtmpfsd, &err, "ds_devtmpfs")',
            "shmem_init_fs_context",
            "kill_litter_super",
            "public_dev_mount",
            "reconfigure_single",
            "devtmpfs_submit_req",
        ):
            self.assertIn(needle, self.devtmpfs_c, needle)

    def test_module_refuses_to_shadow_native_devtmpfs(self):
        self.assertIn('get_fs_type("devtmpfs")', self.main_c)
        self.assertIn("-EEXIST", self.main_c)

    def test_module_activation_is_opt_in(self):
        self.assertIn("module_param(activate, bool, 0400)", self.main_c)
        self.assertIn("if (!activate)", self.main_c)

    def test_no_fake_native_switch(self):
        # The gap is config/observation based; a "native=1" knob would be a
        # fake claim, not an implementation change.
        combined = self.main_c + self.devtmpfs_c
        self.assertNotIn("module_param(native", combined)
        self.assertNotIn("native=", combined)

    def test_hook_teardown_is_explicit(self):
        self.assertIn("stop_machine(install_stopped", self.hooks_c)
        self.assertIn("synchronize_rcu()", self.hooks_c)
        self.assertIn("ds_hooks_uninstall", self.hooks_c)

    def test_status_row_stays_development_r(self):
        rows = []
        for line in STATUS_MD.read_text(encoding="utf-8").splitlines():
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if cells and cells[0] == "DEVTMPFS":
                rows.append(cells)
        self.assertEqual(len(rows), 1, "expected exactly one DEVTMPFS row")
        level, status = rows[0][1], rows[0][2]
        self.assertEqual(level, "R")
        self.assertEqual(status, "development")
        self.assertIn("E 路线", rows[0][3])
        self.assertIn("Action", rows[0][3])

    def test_readme_documents_e_route_and_gaps(self):
        text = README.read_text(encoding="utf-8")
        for phrase in (
            "CONFIG_DEVTMPFS",
            "route E",
            "R / development",
            "trigger ordering",
            "credentials",
            "One Action can verify",
        ):
            self.assertIn(phrase, text, phrase)


if __name__ == "__main__":
    unittest.main()
