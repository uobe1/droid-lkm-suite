"""Guard the ABI 2 inline-hook migration (suite side).

These are source-shape assertions only: the suite C is not compiled locally.
They lock the contract the feature modules and Kbuild now depend on, and fail
if any old kprobe/kretprobe/data-registry path creeps back in.
"""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "core" / "include" / "dnsk_core.h"
INTERNAL = ROOT / "core" / "dnsk_internal.h"
ADAPTER = ROOT / "core" / "dnsk_hook_api.c"
KBUILD = ROOT / "Kbuild"
TMPFSX = ROOT / "features" / "tmpfsx" / "main.c"
CGPIDS = ROOT / "features" / "cgpids" / "virtual.c"
OLD_IMPL = ROOT / "core" / "dnsk_hook.c"


class CoreAbiTwoTest(unittest.TestCase):
    def test_abi_two_and_old_types_gone(self):
        text = HEADER.read_text()
        self.assertIn("#define DNSK_CORE_ABI 2", text)
        for gone in ("struct dnsk_hook_req", "enum dnsk_hook_type",
                     "DNSK_HOOK_KPROBE", "DNSK_HOOK_KRETPROBE",
                     "DNSK_HOOK_SYSCALL", "DNSK_HOOK_DATA",
                     "hook_add", "hook_del"):
            self.assertNotIn(gone, text, gone)

    def test_inline_req_fields(self):
        text = HEADER.read_text()
        self.assertIn("struct dnsk_inline_hook_req {", text)
        start = text.index("struct dnsk_inline_hook_req {")
        body = text[start:text.index("};", start)]
        for field in ("const char *key;", "const char *symbol;",
                      "struct module *owner;", "void *replacement;",
                      "void **origin_out;"):
            self.assertIn(field, body)

    def test_two_slots_replace_old_offsets_in_order(self):
        text = HEADER.read_text()
        api = text[text.index("struct dnsk_core_api {"):]
        order = ["(*resolve)", "(*hook_install)", "(*hook_uninstall)",
                 "(*feature_add)", "(*feature_del)", "(*log)",
                 "(*patch_pointer)", "(*rw)", "*owner"]
        positions = [api.index(token) for token in order]
        self.assertEqual(positions, sorted(positions), order)
        self.assertIn("int (*hook_install)(const struct dnsk_inline_hook_req *req);", api)
        self.assertIn("int (*hook_uninstall)(const char *key);", api)

    def test_old_implementation_file_removed(self):
        self.assertFalse(OLD_IMPL.exists(), "core/dnsk_hook.c must be gone")

    def test_old_registry_symbols_absent_everywhere(self):
        for path in (INTERNAL, ROOT / "core" / "dnsk_core.c"):
            text = path.read_text()
            for gone in ("dnsk_hook_add_internal", "dnsk_hook_del_internal",
                         "DNSK_MAX_KRETPROBES", "register_kprobe",
                         "register_kretprobe"):
                self.assertNotIn(gone, text, f"{path.name}: {gone}")


class AdapterBackendTest(unittest.TestCase):
    def test_backend_primitives_documented(self):
        text = ADAPTER.read_text()
        self.assertIn("dnsk_patch_text(addr, src, len, true)", text)
        self.assertIn("module_alloc", text)
        self.assertIn("module_memfree", text)
        self.assertIn("retired, never freed", text)
        # tlb_flush and quiesce are non-NULL documented no-ops.
        self.assertIn("dnsk_dh_tlb_flush", text)
        self.assertIn("dnsk_dh_quiesce_begin", text)
        self.assertIn("dnsk_dh_quiesce_end", text)

    def test_install_publishes_origin_before_entry(self):
        text = ADAPTER.read_text()
        start = text.index("int dnsk_hook_install_internal")
        body = text[start:text.index("\n}\n", start)]
        self.assertIn("dh_hook_install_origin", body)
        self.assertIn("origin_out", body)
        self.assertNotIn("dh_hook_origin", body)
        # The fail-closed contract is documented on the public header.
        header = " ".join(HEADER.read_text().split())
        self.assertIn("-EOPNOTSUPP", header)
        self.assertIn("before it publishes the entry", header)
        self.assertIn("-EOPNOTSUPP before writing any text", header)


class FeatureMigrationTest(unittest.TestCase):
    def test_tmpfsx_uses_inline_and_patch_pointer(self):
        text = TMPFSX.read_text()
        for gone in ("linux/kprobes.h", "kprobe", "kretprobe",
                     "DNSK_HOOK_", "hook_add", "hook_del"):
            self.assertNotIn(gone, text, gone)
        self.assertIn("hook_install", text)
        self.assertIn("hook_uninstall", text)
        self.assertIn("patch_pointer", text)
        self.assertIn("dnsk_origin_fill_super", text)
        self.assertIn("dnsk_origin_evict_inode", text)
        self.assertIn("dnsk_origin_init_fs_context", text)

    def test_cgpids_uses_inline_wrapper(self):
        text = CGPIDS.read_text()
        for gone in ("linux/kprobes.h", "kprobe", "kretprobe", "DNSK_HOOK_"):
            self.assertNotIn(gone, text, gone)
        self.assertIn("hook_install", text)
        self.assertIn("hook_uninstall", text)
        self.assertIn("static __nocfi int dnsk_migrate_execute", text)
        self.assertIn("dnsk_migrate_pre", text)
        self.assertIn("dnsk_migrate_post", text)
        # The pre-accounting and the failure rollback must both survive.
        self.assertIn("dnsk_pids_uncharge(src, count)", text)
        self.assertIn("dnsk_pids_charge(dnsk_migrate_recs[i].src", text)

    def test_replacements_are_nocfi(self):
        """The origin block has no kCFI hash, so the call back must not be
        checked; every replacement that calls an origin is __nocfi."""
        expected = {
            TMPFSX: (
                "static __nocfi int dnsk_fill_super(",
                "static __nocfi int dnsk_init_fs_context(",
                "static __nocfi void dnsk_evict_inode(",
            ),
            CGPIDS: ("static __nocfi int dnsk_migrate_execute(",),
        }
        for path, decls in expected.items():
            text = path.read_text()
            for decl in decls:
                self.assertIn(decl, text, f"{path.name}: {decl}")
            self.assertIn("#include <linux/compiler.h>", text)


class KbuildEmbeddingTest(unittest.TestCase):
    def test_kbuild_compiles_droid_hook_by_path(self):
        text = KBUILD.read_text()
        self.assertIn("DNSK_HOOK_DIR", text)
        self.assertIn("droid-hook", text)
        self.assertIn("core/dnsk_hook_api.o", text)
        self.assertNotIn("core/dnsk_hook.o", text)
        for wrapper in ("dnsk_dh_rw", "dnsk_dh_txn", "dnsk_dh_insn",
                        "dnsk_dh_hook", "dnsk_dh_shadow", "dnsk_dh_preflight"):
            self.assertIn(f"core/{wrapper}.o", text)
            self.assertTrue((ROOT / "core" / f"{wrapper}.c").is_file(), wrapper)
        self.assertIn("-I$(DNSK_HOOK_PATH)/include", text)
        self.assertIn("$(error", text)

    def test_makefile_documents_hook_dir_default(self):
        text = (ROOT / "Makefile").read_text()
        self.assertIn("DNSK_HOOK_DIR ?= ../droid-hook", text)


if __name__ == "__main__":
    unittest.main()
