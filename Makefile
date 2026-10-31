# SPDX-License-Identifier: GPL-2.0-only
#
# External-module entry point.  The kernel build system includes Kbuild when
# KERNELRELEASE is set; the targets below are only for the developer shell.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean check

all:
	@test -n "$(KDIR)" || { echo "Set KDIR to a prepared kernel build directory." >&2; exit 1; }
	$(MAKE) -C "$(KDIR)" M="$(PWD)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(PWD)" clean

check:
	./scripts/check-style.sh
