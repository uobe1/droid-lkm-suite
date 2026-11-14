# SPDX-License-Identifier: GPL-2.0-only
#
# External-module entry point.  The kernel build system includes Kbuild when
# KERNELRELEASE is set; the targets below are only for the developer shell.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
DOCS_OUT ?= build/docs
# kernel-doc ships inside a kernel tree; CI has none, so allow a direct path.
KERNEL_DOC ?= $(KDIR)/scripts/kernel-doc

.PHONY: all clean check docs docs-api docs-userspace docs-kernel docs-clean

all:
	@test -n "$(KDIR)" || { echo "Set KDIR to a prepared kernel build directory." >&2; exit 1; }
	$(MAKE) -C "$(KDIR)" M="$(PWD)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(PWD)" clean

check:
	./scripts/check-style.sh

# ---------------------------------------------------------------------------
# API documentation
#
# Both halves are generated here, not "documented as generated".  A comment that
# says "produced by tool X" while nothing runs tool X is worse than no comment:
# it sends the reader looking for an artifact that never existed.
#
#   make docs            both halves
#   make docs-userspace  Doxygen over tools/ and core/include/
#   make docs-kernel     kernel-doc over core/, needs KDIR
# ---------------------------------------------------------------------------
docs: docs-kernel docs-userspace

docs-userspace:
	@command -v doxygen >/dev/null 2>&1 || { \
		echo "doxygen not found. Install it, or use the Codespace/CI image." >&2; \
		echo "This target is what produces site/api/userspace/; without the tool" >&2; \
		echo "there is no artifact, and that is the honest state." >&2; \
		exit 1; }
	./scripts/gen-userspace-api.py $(DOCS_OUT)/userspace/index.md
	@mkdir -p $(DOCS_OUT)/userspace
	doxygen Doxyfile
	@echo "userspace API -> $(DOCS_OUT)/userspace/index.md (+ doxygen html)"

# kernel-doc ships with the kernel source tree, so it needs KDIR to be a real
# prepared tree rather than only the external-module build dir.
docs-kernel:
	@test -x "$(KERNEL_DOC)" || { \
		echo "kernel-doc not found at $(KERNEL_DOC)" >&2; \
		echo "Set KERNEL_DOC to the script, or KDIR to a kernel tree that has it." >&2; \
		exit 1; }
	@mkdir -p $(DOCS_OUT)/kernel
	# kernel-doc has no -html mode.  It supports -man, -rst and -none only; passing
	# anything else prints "Argument unknown!" and exits non-zero, which silently
	# produced a 0-byte page.  -rst is the real output mode, and Sphinx turns the
	# rST into HTML (see docs/conf.py wiring in the pages workflow).
	"$(KERNEL_DOC)" -rst \
		core/include/dnsk_core.h core/include/dnsk_abi.h \
		core/include/dnsk_fs_layout.h core/include/dnsk_ns_layout.h \
		core/include/dnsk_xt_layout.h \
		> $(DOCS_OUT)/kernel/index.rst
	@echo "kernel API -> $(DOCS_OUT)/kernel/index.md"
	# The committed page is generated, not hand-written: readers get HTML without
	# having to build anything, and CI re-runs this so it cannot silently drift
	# away from the headers.
	./scripts/gen-kernel-api.py $(DOCS_OUT)/kernel/index.rst $(DOCS_OUT)/kernel/index.md

docs-clean:
	rm -rf $(DOCS_OUT)
