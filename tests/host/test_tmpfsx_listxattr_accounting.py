# SPDX-License-Identifier: GPL-3.0-only
"""Model the tmpfsx listxattr size/fill accounting contract.

Scope: this is a *model* of two implementations of fs/xattr.c's
simple_xattr_list(), not a kernel execution:

  * "device" - the GKI 6.12.30 build the device runs: the function enters
    its simple_xattr rb-tree walk without resetting `err`, so with an empty
    tree it returns the LSM label length instead of the full list length;
  * "fixed"  - the ACK tree the native oracle is built from, which resets
    `err` before the walk.

features/tmpfsx/xattr.c compensates for the former with a single adjustment
function used by *both* the sizing call and the fill call.  The invariant
these tests pin is the one a userspace probe observes:

    listxattr(path, NULL, 0) == listxattr(path, buf, N) == bytes of names

for every combination of cached ACLs, LSM label and simple_xattr entries,
on both kernel variants.  They do NOT execute kernel code; runtime proof
still needs the QEMU-oracle/device differential Action.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
XATTR_C = ROOT / "features" / "tmpfsx" / "xattr.c"

ACL_ACCESS = "system.posix_acl_access"    # sizeof() == 24
ACL_DEFAULT = "system.posix_acl_default"  # sizeof() == 25
SELINUX = "security.selinux"              # sizeof() == 16
TRUSTED = "trusted.overlay.opaque"
USER = "user.probe"


def name_len(name):
    return len(name.encode()) + 1


SELINUX_LEN = name_len(SELINUX)  # sizeof(XATTR_NAME_SELINUX)


class KernelModel:
    """fs/xattr.c simple_xattr_list(), both device and fixed variants."""

    def __init__(self, acl_names, lsm_len, tree_names, privileged=True):
        self.acl_names = list(acl_names)
        self.lsm_len = lsm_len
        self.tree_names = list(tree_names)
        self.privileged = privileged

    def listed_names(self):
        names = list(self.acl_names)
        if self.lsm_len:
            names.append(SELINUX)
        for name in self.tree_names:
            if not self.privileged and name.startswith("trusted."):
                continue
            if name.startswith("security.") and name == SELINUX:
                continue  # MAC label already provided by the LSM
            names.append(name)
        return names

    def expected_bytes(self):
        return sum(name_len(n) for n in self.listed_names())

    def call(self, size, fixed):
        """Return (return value, bytes written) for one listxattr call."""
        has_buffer = size != 0          # the listxattr(2) wrapper passes NULL iff size == 0
        remaining = size
        written = 0
        err = 0

        # posix_acl_listxattr()
        for name in self.acl_names:
            length = name_len(name)
            if has_buffer and remaining < length:
                return -34, written     # -ERANGE, before any write of this name
            if has_buffer:
                written += length
            remaining -= length

        # security_inode_listsecurity()
        err = self.lsm_len
        if has_buffer and remaining < err:
            return -34, written         # -ERANGE
        if has_buffer:
            written += err
        remaining -= err

        if fixed:
            err = 0

        # rb-tree walk
        for name in self.tree_names:
            if not self.privileged and name.startswith("trusted."):
                continue
            if name == SELINUX:
                continue
            length = name_len(name)
            if has_buffer and remaining < length:
                return -34, written
            if has_buffer:
                written += length
            remaining -= length
            err = 0

        return (err if err else size - remaining), written


def module_wrapper(model, size, fixed):
    """dnsk_tmpfsx_listxattr(): kernel call + dnsk_listxattr_account()."""
    ret, written = model.call(size, fixed)
    if ret < 0:
        return ret, written
    acl_bytes = sum(name_len(n) for n in model.acl_names)   # i_acl / i_default_acl
    if acl_bytes and ret == model.lsm_len:                  # security_inode_listsecurity(inode, NULL, 0)
        return ret + acl_bytes, written
    return ret, written


class TmpfsxListxattrAccountingTest(unittest.TestCase):
    def cases(self):
        trees = [
            [],
            [USER],
            [SELINUX],                       # MAC label only: skipped, tree "lists" nothing
            [TRUSTED],                       # skipped for unprivileged callers
            [USER, SELINUX],
        ]
        for acl in ([], [ACL_ACCESS], [ACL_DEFAULT], [ACL_ACCESS, ACL_DEFAULT]):
            for lsm in (0, SELINUX_LEN):
                for tree in trees:
                    yield KernelModel(acl, lsm, tree)

    def test_sizing_fill_and_written_bytes_agree(self):
        for model in self.cases():
            for fixed in (False, True):
                with self.subTest(acl=model.acl_names, lsm=model.lsm_len,
                                  tree=model.tree_names, fixed=fixed):
                    size_rv, size_written = module_wrapper(model, 0, fixed)
                    self.assertGreaterEqual(size_rv, 0)
                    self.assertEqual(size_written, 0)

                    fill_rv, fill_written = module_wrapper(model, size_rv, fixed)
                    self.assertEqual(fill_rv, size_rv,
                                     "fill return must equal the size reported by sizing")
                    self.assertNotEqual(fill_rv, -34, "fill must never hit ERANGE")
                    self.assertEqual(fill_written, size_rv,
                                     "fill must write exactly the reported size")
                    self.assertEqual(size_rv, model.expected_bytes(),
                                     "reported size must equal the listed name bytes")

    def test_oversized_fill_still_reports_listed_bytes(self):
        """A caller may pass a larger buffer than the sizing call asked for."""
        for model in self.cases():
            for fixed in (False, True):
                with self.subTest(acl=model.acl_names, lsm=model.lsm_len,
                                  tree=model.tree_names, fixed=fixed):
                    size_rv, _ = module_wrapper(model, 0, fixed)
                    big_rv, big_written = module_wrapper(model, size_rv + 4096, fixed)
                    self.assertEqual(big_rv, size_rv)
                    self.assertEqual(big_written, size_rv)

    def test_compensation_is_inert_on_fixed_kernels(self):
        for model in self.cases():
            with self.subTest(acl=model.acl_names, lsm=model.lsm_len,
                              tree=model.tree_names):
                self.assertEqual(module_wrapper(model, 0, True),
                                 module_wrapper(model, 0, False))

    def test_no_adjustment_when_tree_contributes_a_name(self):
        model = KernelModel([ACL_DEFAULT], SELINUX_LEN, [USER])
        ret, _ = module_wrapper(model, 0, False)
        self.assertEqual(ret, model.expected_bytes())
        raw, _ = model.call(0, False)
        self.assertEqual(ret, raw, "a listed tree entry already makes the kernel total correct")

    def test_unprivileged_trusted_entry_does_not_hide_acl_bytes(self):
        model = KernelModel([ACL_DEFAULT], 0, [TRUSTED])
        model.privileged = False
        ret, _ = module_wrapper(model, 0, False)
        self.assertEqual(ret, name_len(ACL_DEFAULT))


class TmpfsxSourceGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = XATTR_C.read_text(encoding="utf-8")

    def test_single_accounting_point_used_by_both_passes(self):
        self.assertEqual(
            len(re.findall(r"dnsk_listxattr_account\(", self.source)), 2,
            "accounting must be one helper called from one place",
        )
        self.assertNotRegex(
            self.source,
            r"dnsk_tmpfsx_listxattr[\s\S]*?if\s*\(\s*!?\s*buffer\s*\)",
            "sizing and fill must share the same path; no buffer-conditional accounting",
        )

    def test_accounting_checks_the_kernel_bug_signature(self):
        self.assertIn("ret != lsm_bytes", self.source)
        self.assertIn("security_inode_listsecurity", self.source)
        self.assertIn("XATTR_NAME_POSIX_ACL_ACCESS", self.source)
        self.assertIn("XATTR_NAME_POSIX_ACL_DEFAULT", self.source)


if __name__ == "__main__":
    unittest.main()
