# SPDX-License-Identifier: GPL-2.0-only
#
# External-module entry point.  The kernel build system includes Kbuild when
# KERNELRELEASE is set; the targets below are only for the developer shell.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean check docs docs-check docs-clean

all:
	@test -n "$(KDIR)" || { echo "Set KDIR to a prepared kernel build directory." >&2; exit 1; }
	$(MAKE) -C "$(KDIR)" M="$(PWD)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(PWD)" clean

check: docs-check
	sh ./scripts/check-style.sh

# ---------------------------------------------------------------------------
# API documentation
#
# There is exactly one source of truth: the documentation comments in
# core/include/*.h (kernel-doc dialect) and in tools/dnskctl /
# tools/dnsk_preflight.c (Doxygen dialect).  scripts/gen-api-docs.py reads
# those comments only; it never parses a format string or a usage() here-doc.
# It is pure Python3 because the dev container has neither kernel-doc nor
# doxygen and the hard constraint forbids installing them locally.
#
#   make docs        regenerate docs/api/kernel.md and docs/api/userspace.md
#   make docs-check  fail when the committed pages drift from the comments
#
# The committed pages are what CI checks; readers never have to build docs.
# ---------------------------------------------------------------------------
docs:
	python3 scripts/gen-api-docs.py

docs-check:
	python3 scripts/gen-api-docs.py --check

docs-clean:
	rm -rf build/docs
