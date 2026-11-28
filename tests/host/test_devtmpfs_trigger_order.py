# SPDX-License-Identifier: GPL-3.0-only
"""Ordering-logic guard for the opt-in ``trigger=bus`` device lifecycle.

Scope: this test exercises the *decision logic* and the *source call order*,
not real kernel timing. It

  1. extracts the pure ``ds_trigger_plan()`` block from
     ``features/devtmpfs/vendor/devtmpfs/trigger_plan.h``, compiles the real C
     code with the host gcc and drives it through ctypes, so a change to the
     plan bits makes the test fail;
  2. reads the ``ds_bus_notify()`` wrapper body from hooks.c and asserts the
     call order ``plan -> create -> original bus_notify -> delete``;
  3. pins the opt-in default: ``trigger = "entry"``, the charp module
     parameter, the entry/bus hook selection, and the absence of an
     unconditional bus log.

Real kernel timing (node present when the bus notifier chain / uevent runs)
needs the Action probe described in features/devtmpfs/README.md. This test
cannot and does not claim that.
"""

import ctypes
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "features" / "devtmpfs" / "vendor" / "devtmpfs" / "trigger_plan.h"
HOOKS_C = ROOT / "features" / "devtmpfs" / "vendor" / "devtmpfs" / "hooks.c"
MAIN_C = ROOT / "features" / "devtmpfs" / "vendor" / "devtmpfs" / "main.c"

PURE_BEGIN = "/* DNSK_TRIGGER_PURE_BEGIN */"
PURE_END = "/* DNSK_TRIGGER_PURE_END */"
WRAP_BEGIN = "/* DNSK_BUS_NOTIFY_BEGIN */"
WRAP_END = "/* DNSK_BUS_NOTIFY_END */"

PLAN_NONE = 0
PLAN_CREATE_BEFORE = 1
PLAN_DELETE_AFTER = 2

_STATE = {}


def _between(text, begin, end):
    i = text.find(begin)
    j = text.find(end)
    if i < 0 or j < 0 or j <= i:
        raise AssertionError(f"markers missing/ordered wrong: {begin} / {end}")
    return text[i + len(begin):j]


def setUpModule():
    tmp = tempfile.TemporaryDirectory()
    _STATE["tmp"] = tmp
    d = pathlib.Path(tmp.name)

    block = _between(HEADER.read_text(), PURE_BEGIN, PURE_END)
    block = block.replace("static inline ", "")
    shim = ("#include <stdint.h>\n#include <stdbool.h>\n" + block)
    (d / "shim.c").write_text(shim)
    so = d / "libtrigger.so"
    r = subprocess.run(
        ["gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
         "-o", str(so), str(d / "shim.c")],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError("trigger plan failed to compile:\n" + r.stderr)
    lib = ctypes.CDLL(str(so))
    lib.ds_trigger_plan.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.ds_trigger_plan.restype = ctypes.c_uint
    _STATE["lib"] = lib
    _STATE["hooks"] = HOOKS_C.read_text()
    _STATE["main"] = MAIN_C.read_text()
    wrapper = _between(_STATE["hooks"], WRAP_BEGIN, WRAP_END)
    _STATE["wrapper"] = wrapper

    # Compile the real wrapper body with mocked node/notify sinks. This drives
    # the C control flow, not just the source text order.
    sim = """#include <stdbool.h>
#include <stdio.h>
#include "trigger_plan.h"
struct device { int dummy; };
#define BUS_NOTIFY_ADD_DEVICE 0
#define BUS_NOTIFY_DEL_DEVICE 1
#define dev_name(d) "mock"
#define pr_info(fmt, ...) ((void)0)
static bool bus_trigger;
static int order[16];
static int n;
static void mock_update(struct device *d, bool create)
{
	order[n++] = create ? 1 : 2;
}
static void mock_original(struct device *d, unsigned int value)
{
	order[n++] = 3;
}
#define ds_devtmpfs_update_node mock_update
#define ds_bus_notify_original mock_original
""" + wrapper + """
int main(void)
{
	struct device d;

	ds_bus_notify(&d, BUS_NOTIFY_ADD_DEVICE);
	printf("add");
	for (int i = 0; i < n; i++)
		printf(" %d", order[i]);
	printf("\\n");
	n = 0;
	ds_bus_notify(&d, BUS_NOTIFY_DEL_DEVICE);
	printf("del");
	for (int i = 0; i < n; i++)
		printf(" %d", order[i]);
	printf("\\n");
	n = 0;
	ds_bus_notify(&d, 2);
	printf("other");
	for (int i = 0; i < n; i++)
		printf(" %d", order[i]);
	printf("\\n");
	return 0;
}
"""
    (d / "sim.c").write_text(sim)
    exe = d / "sim"
    r = subprocess.run(
        ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra",
         "-I", str(HEADER.parent), "-o", str(exe), str(d / "sim.c")],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError("bus wrapper simulation failed:\n" + r.stderr)
    r = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    _STATE["sim"] = r.stdout


def tearDownModule():
    _STATE["tmp"].cleanup()


class TriggerPlanLogicTest(unittest.TestCase):
    """The real C plan function, not a Python re-implementation."""

    def test_add_creates_before_the_notifier(self):
        self.assertEqual(_STATE["lib"].ds_trigger_plan(1, 0),
                         PLAN_CREATE_BEFORE)

    def test_del_deletes_after_the_notifier(self):
        self.assertEqual(_STATE["lib"].ds_trigger_plan(0, 1),
                         PLAN_DELETE_AFTER)

    def test_other_events_have_no_node_action(self):
        self.assertEqual(_STATE["lib"].ds_trigger_plan(0, 0), PLAN_NONE)

    def test_add_and_del_are_distinct_bits(self):
        self.assertEqual(_STATE["lib"].ds_trigger_plan(1, 1),
                         PLAN_CREATE_BEFORE | PLAN_DELETE_AFTER)


class BusWrapperOrderTest(unittest.TestCase):
    """Mocked execution order: create -> original -> delete around bus_notify()."""

    def test_mocked_wrapper_add_del_other_order(self):
        # 1 = create, 2 = delete, 3 = original bus_notify chain.
        self.assertIn("add 1 3", _STATE["sim"])
        self.assertIn("del 3 2", _STATE["sim"])
        self.assertIn("other 3", _STATE["sim"])

    def test_wrapper_calls_plan_then_original(self):
        body = _STATE["wrapper"]
        plan = body.index("ds_trigger_plan(")
        create = body.index("ds_devtmpfs_update_node(dev, true)")
        original = body.index("ds_bus_notify_original(dev, value)")
        delete = body.index("ds_devtmpfs_update_node(dev, false)")
        self.assertLess(plan, create)
        self.assertLess(create, original)
        self.assertLess(original, delete)

    def test_create_and_delete_are_conditioned_on_plan_bits(self):
        body = _STATE["wrapper"]
        original = body.index("ds_bus_notify_original(dev, value)")
        self.assertLess(
            body.index("if (plan & DS_TRIGGER_CREATE_BEFORE)"), original)
        self.assertGreater(
            body.index("if (plan & DS_TRIGGER_DELETE_AFTER)"), original)

    def test_original_bus_notify_is_called_exactly_once(self):
        self.assertEqual(_STATE["wrapper"].count("ds_bus_notify_original("), 1)


class TriggerOptInTest(unittest.TestCase):
    def test_default_is_entry(self):
        self.assertIn('static char *trigger = "entry";', _STATE["main"])
        self.assertIn("module_param(trigger, charp, 0400);", _STATE["main"])

    def test_trigger_is_validated_and_selected(self):
        main = _STATE["main"]
        self.assertIn('strcmp(trigger, "entry")', main)
        self.assertIn('strcmp(trigger, "bus")', main)
        self.assertIn("ds_hooks_prepare(bus_trigger)", main)

    def test_hook_selection_is_entry_or_bus_not_both(self):
        hooks = _STATE["hooks"]
        self.assertIn('"bus_notify"', hooks)
        self.assertIn('!strcmp(hooks[i].name, "bus_notify")', hooks)
        self.assertIn('strcmp(hooks[i].name, "bus_notify")', hooks)
        self.assertIn("active_hooks[active_count++]", hooks)

    def test_bus_mode_is_observable_but_not_unconditional(self):
        main = _STATE["main"]
        self.assertIn('pr_info("trigger=bus active\\n")', main)
        self.assertIn('if (bus_trigger)', main)
        self.assertIn('trigger=bus active', main)
        self.assertIn("bus trigger: add %s before-notify", _STATE["hooks"])
        self.assertIn("bus trigger: del %s after-notify", _STATE["hooks"])


if __name__ == "__main__":
    unittest.main()
