# SPDX-License-Identifier: GPL-2.0-only
#
# External-module entry point.  The kernel build system includes Kbuild when
# KERNELRELEASE is set; the targets below are only for the developer shell.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
DOCS_OUT ?= build/docs

.PHONY: all clean check docs docs-userspace docs-kernel docs-clean

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
docs: docs-userspace docs-kernel

docs-userspace:
	@command -v doxygen >/dev/null 2>&1 || { \
		echo "doxygen not found. Install it, or use the Codespace/CI image." >&2; \
		echo "This target is what produces site/api/userspace/; without the tool" >&2; \
		echo "there is no artifact, and that is the honest state." >&2; \
		exit 1; }
	doxygen Doxyfile
	@echo "userspace API -> $(DOCS_OUT)/doxygen-userspace/html/index.html"

# kernel-doc ships with the kernel source tree, so it needs KDIR to be a real
# prepared tree rather than only the external-module build dir.
docs-kernel:
	@test -x "$(KDIR)/scripts/kernel-doc" || { \
		echo "kernel-doc not found at $(KDIR)/scripts/kernel-doc" >&2; \
		echo "Set KDIR to a kernel source/build tree that has it (a DDK kdir does)." >&2; \
		exit 1; }
	@mkdir -p $(DOCS_OUT)/kernel
	# kernel-doc has no -html mode.  It supports -man, -rst and -none only; passing
	# anything else prints "Argument unknown!" and exits non-zero, which silently
	# produced a 0-byte page.  -rst is the real output mode, and Sphinx turns the
	# rST into HTML (see docs/conf.py wiring in the pages workflow).
	"$(KDIR)/scripts/kernel-doc" -rst \
		core/include/dnsk_core.h core/include/dnsk_abi.h \
		> $(DOCS_OUT)/kernel/index.rst
	@echo "kernel API -> $(DOCS_OUT)/kernel/index.rst"

docs-clean:
	rm -rf $(DOCS_OUT)
