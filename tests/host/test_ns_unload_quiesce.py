# SPDX-License-Identifier: GPL-3.0-only
"""Ordering / counter guard for unloading dnsk_ns with concurrent callers.

This is the regression test for the real-device panic (see
docs/evidence/device-panic-dnsk-ns-unload-2026-11-27.md): a task that was
still *inside* a module-resident path (fork/clone into a new pid/ipc namespace,
a blocking SYSVIPC/POSIX-mqueue syscall, or the module's full ``do_exit``
replacement) when ``rmmod dnsk_ns`` freed the module text.  ``stop_machine()``
only covers the instant the entry is restored and ``synchronize_rcu()`` only
covers RCU read sections; neither waits for a task parked in module text.

Two layers are pinned here:

1. A *model* of the unload protocol that a bare ``rmmod`` cannot bypass:
   every sleep-capable module entry holds an observable in-flight reference
   (and a module reference), so ``module_exit`` is provably entered only when
   ``inflight == 0``; quiesce blocks new entries, takes an RCU grace, then
   waits for the counter to drain.
2. Source guards on the C, so the model's premises cannot silently rot:
   the counter exists and is wired into the lifecycle + blocking-syscall
   entries, ``ds_port_exit`` quiesces *before* it unhooks, and
   ``ds_ipc_backend_exit`` no longer WARNs-and-tears-down on live task state.

The model reproduces the historical failure (freeing while a caller is still
in module text => panic) and proves the new order forbids it.  It is a model,
not kernel execution: only the QEMU/device differential can prove runtime
behaviour, but this fails loudly if the ordering regresses.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "features" / "ns" / "vendor" / "port"
IPC_TASK_C = PORT / "ipc-task.c"
IPC_BACKEND_H = PORT / "ipc-backend.h"
MAIN_C = PORT / "main.c"
FORK_C = PORT / "fork.c"
NSPROXY_C = PORT / "nsproxy.c"
PID_ALLOC_C = PORT / "pid_alloc.c"
EXIT_C = PORT / "exit.c"
MSG_C = PORT / "msg.c"
MQUEUE_C = PORT / "mqueue.c"
SEM_C = PORT / "sem.c"
SHM_C = PORT / "shm.c"
CORE_HOOK_C = ROOT / "core" / "dnsk_hook_api.c"


# --------------------------------------------------------------------------
# 1. Model of the unload protocol.
# --------------------------------------------------------------------------
class Module:
    """Very small model of the parts of the module that matter here.

    ``inflight`` mirrors ``ds_ns_inflight`` and the module refcount at once:
    a nonzero value is what makes ``rmmod`` answer EAGAIN.  ``text_freed``
    records the moment the module text is unmapped.
    """

    def __init__(self):
        self.inflight = 0
        self.quiescing = False
        self.text_freed = False
        self.panicked = False

    # --- entry side (ds_ns_enter / ds_ns_leave) -------------------------
    def enter(self):
        """Sleep-capable entry.  Refused once quiescing / module going."""
        if self.quiescing:
            return False
        self.inflight += 1
        # re-check after publish, exactly like ds_ns_enter's smp_mb window
        if self.quiescing:
            self.inflight -= 1
            return False
        return True

    def leave(self):
        assert self.inflight > 0, "unbalanced ds_ns_leave"
        self.inflight -= 1

    # --- rmmod / module_exit -------------------------------------------
    def rmmod(self):
        """delete_module(2): refuses while refcount != 0."""
        if self.inflight != 0:
            return "EAGAIN"
        return "ok"

    def module_exit(self, order):
        """Run the exit path.  ``order`` is 'new' or 'old'."""
        if order == "old":
            # historical bug: unhook + free first, quiesce/live accounting
            # only afterwards (ds_ipc_backend_exit's WARN-and-continue).
            self.unhook_and_free()
            self.quiesce()
        else:
            assert self.quiesce() == 0
            self.unhook_and_free()

    def quiesce(self):
        self.quiescing = True
        # synchronize_rcu() would be here; the model only needs the drain.
        if self.inflight != 0:
            return -16  # -EBUSY
        return 0

    def unhook_and_free(self):
        if self.inflight != 0:
            # a task still executing / parked in module text: its epilogue
            # or its wake-up return lands in freed text -> IABT.
            self.panicked = True
        self.text_freed = True

    # --- a caller parked inside module text ----------------------------
    def in_module_call(self):
        assert self.enter()
        return True

    def finishes(self):
        """Caller wakes up and returns out of module text."""
        if self.text_freed:
            self.panicked = True
        self.leave()


class UnloadProtocolModelTest(unittest.TestCase):
    def test_old_order_panics_when_a_caller_is_still_in_module(self):
        m = Module()
        m.in_module_call()                 # e.g. blocked in msgrcv / fork
        self.assertEqual(m.rmmod(), "EAGAIN")
        # Simulate the historical bypass: rmmod was still attempted and text
        # freed while the caller was parked.
        m.text_freed = True
        m.finishes()
        self.assertTrue(m.panicked, "old order must model the panic")

    def test_new_order_waits_for_inflight_before_freeing(self):
        m = Module()
        m.in_module_call()
        self.assertEqual(m.rmmod(), "EAGAIN",
                         "in-flight call must make rmmod refuse")
        m.finishes()                       # caller leaves; count hits zero
        self.assertEqual(m.rmmod(), "ok")
        m.module_exit("new")
        self.assertEqual(m.inflight, 0)
        self.assertFalse(m.panicked)

    def test_quiesce_refuses_while_inflight_and_reaches_zero_after(self):
        m = Module()
        calls = [m.in_module_call() for _ in range(12)]  # the tasks=12 case
        self.assertEqual(m.quiesce(), -16)  # -EBUSY, do not free yet
        self.assertFalse(m.text_freed)
        for _ in calls:
            m.finishes()
        self.assertEqual(m.inflight, 0, "counter must be observable and zero")
        self.assertEqual(m.quiesce(), 0)
        m.unhook_and_free()
        self.assertFalse(m.panicked)

    def test_new_entry_during_quiesce_is_refused_not_counted(self):
        m = Module()
        m.quiescing = True
        self.assertFalse(m.enter(),
                         "a new fork/clone/syscall must not enter module text")
        self.assertEqual(m.inflight, 0)

    def test_concurrent_fork_clone_window(self):
        """fork/clone racing rmmod: rmmod cannot proceed until every clone
        that already entered module text has left."""
        m = Module()
        cloned = [m.in_module_call() for _ in range(5)]
        self.assertEqual(m.rmmod(), "EAGAIN")
        # three finish, two still inside => still unsafe
        for _ in range(3):
            m.finishes()
        self.assertEqual(m.rmmod(), "EAGAIN")
        self.assertEqual(m.inflight, 2)
        for _ in range(2):
            m.finishes()
        self.assertEqual(m.rmmod(), "ok")
        self.assertEqual(m.inflight, 0)


# --------------------------------------------------------------------------
# 2. Source guards: the model above must describe the real C.
# --------------------------------------------------------------------------
class UnloadSourceGuardTest(unittest.TestCase):
    def setUp(self):
        self.ipc_task = IPC_TASK_C.read_text()
        self.backend_h = IPC_BACKEND_H.read_text()
        self.main = MAIN_C.read_text()

    def test_inflight_counter_is_real_and_module_pinned(self):
        self.assertIn("atomic_t ds_ns_inflight", self.ipc_task)
        # The observable is not a comment: the entry takes a module reference
        # so a bare rmmod answers EAGAIN while a call is parked in text.
        self.assertIn("try_module_get(THIS_MODULE)", self.ipc_task)
        self.assertIn("module_put(THIS_MODULE)", self.ipc_task)

    def test_entry_helpers_declared_and_defined(self):
        for sym in ("ds_ns_enter", "ds_ns_leave", "ds_ns_hold",
                    "ds_ns_unhold", "ds_ns_quiesce"):
            self.assertIn(sym, self.backend_h, f"{sym} not declared")
            self.assertIn(sym, self.ipc_task, f"{sym} not defined")

    def test_quiesce_waits_rcu_then_drain(self):
        self.assertIn("synchronize_rcu()", self.ipc_task)
        self.assertIn("wait_event_timeout", self.ipc_task)
        self.assertIn("atomic_read(&ds_ns_inflight) == 0", self.ipc_task)

    def test_port_exit_quiesces_before_unhook(self):
        body = self.main[self.main.index("static void __exit ds_port_exit"):]
        i_q = body.index("ds_ns_quiesce(")
        i_unhook = body.index("ds_hooks_uninstall(")
        i_backend = body.index("ds_ipc_backend_exit(")
        self.assertLess(i_q, i_unhook,
                        "quiesce must run before hooks are removed")
        self.assertLess(i_unhook, i_backend,
                        "IPCs teardown must follow the hook restore")
        # The old "warn and keep dismantling" gate must be gone.
        self.assertNotIn("unloading with live namespaces", self.main)

    def test_lifecycle_and_blocking_entries_are_instrumented(self):
        # fork/clone path
        fork = FORK_C.read_text()
        self.assertIn("ds_ns_enter", fork)
        self.assertIn("__ds_kernel_clone", fork)
        self.assertIn("__ds_ksys_unshare", fork)
        nsproxy = NSPROXY_C.read_text()
        # copy_namespaces/free_nsproxy/... are names.h macros, so the body
        # symbol is the un-macroed __copy_namespaces etc.
        self.assertIn("__copy_namespaces", nsproxy)
        self.assertIn("__free_nsproxy", nsproxy)
        self.assertIn("__unshare_nsproxy_namespaces", nsproxy)
        self.assertIn("__exec_task_namespaces", nsproxy)
        self.assertIn("ds_ns_enter", nsproxy)
        # pid allocation (may sleep in kmem/idr)
        self.assertIn("__alloc_pid", PID_ALLOC_C.read_text())
        self.assertIn("ds_ns_enter", PID_ALLOC_C.read_text())
        # full do_exit replacement: never refused, only tracked
        self.assertIn("ds_ns_hold", EXIT_C.read_text())
        self.assertIn("ds_ns_unhold", EXIT_C.read_text())
        # blocking SYSVIPC / mqueue syscalls
        for f in (MSG_C, MQUEUE_C, SEM_C, SHM_C):
            self.assertIn("ds_ns_enter", f.read_text(),
                          f"{f.name} blocking entry not instrumented")

    def test_backend_exit_no_longer_warns_and_tears_down(self):
        body = self.ipc_task
        self.assertNotIn("WARN_ON(!hash_empty(task_states))", body,
                         "the unload gate must not WARN-and-continue")
        # It must depend on the quiesce flag rather than free blindly.
        self.assertIn("ds_ns_quiescing", body)

    def test_core_hook_exit_drains_inflight(self):
        core = CORE_HOOK_C.read_text()
        # dnsk_hooks_exit() must take a grace period like dnsk_hook_del_internal.
        exit_fn = core[core.index("void dnsk_hooks_exit(void)"):]
        self.assertIn("synchronize_rcu()", exit_fn.split("\n}\n", 1)[0])


if __name__ == "__main__":
    unittest.main()
