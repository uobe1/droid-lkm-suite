# SPDX-License-Identifier: GPL-3.0-only
"""Model the dnsk_ns IPC-namespace backfill contract.

Scope: this mirrors the two C functions added to
features/ns/vendor/port/ipc.c (`ds_ipc_backfill_install` /
`ds_ipc_backfill_restore`) plus the `validate_nsset()` resolution in
vendor/port/nsproxy.c.  It is a *model*, not kernel execution: it can show
that a task created before the module resolves to the initial IPC namespace
and stays joinable through teardown, but only the QEMU-oracle/device
differential Action can prove the runtime behaviour.

The property pinned here is the one a userspace probe observes:

    pidfd setns(CLONE_NEWIPC) on a pre-load task succeeds, and the reference
    bookkeeping of the backfill balances across load/join/exit/unload.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IPC_C = ROOT / "features" / "ns" / "vendor" / "port" / "ipc.c"
NSPROXY_C = ROOT / "features" / "ns" / "vendor" / "port" / "nsproxy.c"
MAIN_C = ROOT / "features" / "ns" / "vendor" / "port" / "main.c"


class InitialIpcNs:
    """The backend's init_ipc_ns: one initial reference, like ipc.c:26."""

    def __init__(self):
        self.refs = 1

    def get(self):
        self.refs += 1
        return self

    def put(self):
        self.refs -= 1
        assert self.refs > 0, "initial IPC namespace over-released"


class NsProxy:
    def __init__(self, ipc_ns=None):
        self.ipc_ns = ipc_ns


def backfill_install(nsproxies, init_ns):
    """One reference per distinct nsproxy still holding NULL, see ipc.c."""
    filled = 0
    for nsproxy in nsproxies:                 # tasklist_lock write excludes forks
        if nsproxy.ipc_ns is None:
            nsproxy.ipc_ns = init_ns.get()
            filled += 1
    return filled


def backfill_restore(nsproxies, init_ns, init_nsproxy=None):
    """Clear + put every nsproxy that still points at the initial namespace."""
    cleared = 0
    for nsproxy in nsproxies + ([init_nsproxy] if init_nsproxy else []):
        if nsproxy.ipc_ns is init_ns:
            nsproxy.ipc_ns = None
            cleared += 1
    for _ in range(cleared):
        init_ns.put()
    return cleared


def validate_nsset(nsproxy):
    """nsproxy.c: fail closed on a NULL ipc_ns, else the namespace is joinable."""
    return "EINVAL" if nsproxy.ipc_ns is None else "ok"


def install_join(nsproxy, target_ns):
    """ipcns_install(): transfer one reference from the nsproxy to the target."""
    if target_ns is None:
        return "EINVAL"
    nsproxy.ipc_ns.put()          # put_ipc_ns(old)
    nsproxy.ipc_ns = target_ns.get()
    return "ok"


class IpcBackfillModelTest(unittest.TestCase):
    def setUp(self):
        self.init_ns = InitialIpcNs()
        # init_nsproxy plus three pre-load task nsproxies (all NULL on the device)
        self.init_nsproxy = NsProxy()
        self.tasks = [NsProxy() for _ in range(3)]
        self.all = [self.init_nsproxy] + self.tasks

    def test_preload_task_joins_after_backfill(self):
        backfill_install(self.all, self.init_ns)
        for nsproxy in self.all:
            self.assertEqual(validate_nsset(nsproxy), "ok")
            self.assertEqual(install_join(nsproxy, nsproxy.ipc_ns), "ok")

    def test_without_backfill_pidfd_setns_stays_fail_closed(self):
        for nsproxy in self.all:
            self.assertEqual(validate_nsset(nsproxy), "EINVAL")

    def test_shared_nsproxy_takes_one_reference(self):
        shared = NsProxy()
        filled = backfill_install([shared, shared, shared], self.init_ns)
        self.assertEqual(filled, 1)
        self.assertEqual(self.init_ns.refs, 2)

    def test_restore_balances_every_reference(self):
        backfill_install(self.all, self.init_ns)
        self.assertEqual(self.init_ns.refs, 1 + len(self.all))
        cleared = backfill_restore(self.tasks, self.init_ns, self.init_nsproxy)
        self.assertEqual(cleared, len(self.all))
        self.assertEqual(self.init_ns.refs, 1, "one reference per backfilled nsproxy")
        for nsproxy in self.all:
            self.assertIsNone(nsproxy.ipc_ns)

    def test_task_exit_before_unload_drops_its_own_reference(self):
        backfill_install(self.all, self.init_ns)
        # task exits: the hooked free_nsproxy() puts its ipc_ns (ipc.c:256 put)
        self.tasks[0].ipc_ns.put()
        self.tasks[0].ipc_ns = None
        backfill_restore(self.tasks[1:], self.init_ns, self.init_nsproxy)
        self.assertEqual(self.init_ns.refs, 1)

    def test_join_to_private_namespace_is_left_alone_by_restore(self):
        private = InitialIpcNs()
        backfill_install(self.all, self.init_ns)
        joinable = self.tasks[0]
        self.assertEqual(install_join(joinable, private), "ok")
        cleared = backfill_restore(self.tasks, self.init_ns, self.init_nsproxy)
        self.assertEqual(cleared, len(self.all) - 1)
        self.assertIs(joinable.ipc_ns, private, "post-load join must survive unload")
        self.assertEqual(private.refs, 2)      # its own initial + the nsproxy's
        self.assertEqual(self.init_ns.refs, 1)

    def test_new_task_during_quiesce_gets_no_reference(self):
        backfill_install(self.all, self.init_ns)
        # quiesce raised: copy_ipcs() hands new nsproxies NULL instead of a ref
        child = NsProxy()
        backfill_restore(self.tasks, self.init_ns, self.init_nsproxy)
        self.assertIsNone(child.ipc_ns)
        self.assertEqual(self.init_ns.refs, 1)


class IpcBackfillSourceGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ipc_c = IPC_C.read_text(encoding="utf-8")
        cls.nsproxy_c = NSPROXY_C.read_text(encoding="utf-8")
        cls.main_c = MAIN_C.read_text(encoding="utf-8")

    def test_backfills_the_kernels_own_init_nsproxy(self):
        self.assertIn('kallsyms_name_to_addr("init_nsproxy")', self.ipc_c)
        self.assertIn("WRITE_ONCE(ds_init_nsproxy->ipc_ns,",
                      self.ipc_c)
        self.assertIn("for_each_process(task)", self.ipc_c)
        self.assertIn("rcu_read_lock();", self.ipc_c)
        self.assertIn("task_lock(task);", self.ipc_c)
        self.assertNotIn("write_lock_irqsave(&tasklist_lock", self.ipc_c,
                         "walking under tasklist_lock would nest task_lock below it")
        self.assertNotIn("write_lock(&tasklist_lock", self.ipc_c)

    def test_quiesce_guard_is_checked_on_both_allocators(self):
        copy_ipcs = self.ipc_c.split("struct ipc_namespace *copy_ipcs", 1)[1]
        self.assertIn("ds_ipc_quiescing", copy_ipcs.split("}", 1)[0])
        install = self.ipc_c.split("static int ipcns_install", 1)[1]
        self.assertIn("ds_ipc_quiescing", install.split("}", 1)[0])

    def test_restore_runs_before_hooks_are_removed(self):
        self.assertIn("ds_ipc_backfill_install()", self.main_c)
        exit_fn = self.main_c.split("static void __exit ds_port_exit", 1)[1]
        exit_fn = exit_fn.split("module_exit(", 1)[0]
        self.assertLess(
            exit_fn.index("ds_ipc_backfill_restore()"),
            exit_fn.index("ds_hooks_uninstall()"),
            "pointers must be cleared while the hooked copy_ipcs can fail closed",
        )
        self.assertLess(
            exit_fn.index("ds_ipc_backfill_restore()"),
            exit_fn.index("ds_ipc_backend_exit()"),
            "the backend namespace must still be alive when references drop",
        )

    def test_setns_still_fails_closed_on_null(self):
        self.assertIn("if (unlikely(!nsp->ipc_ns))", self.nsproxy_c)
        self.assertIn("ret = -EINVAL;", self.nsproxy_c)


if __name__ == "__main__":
    unittest.main()
