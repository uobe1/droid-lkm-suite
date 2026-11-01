# SPDX-License-Identifier: GPL-2.0-only
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all modules clean
all:
	@test -n "$(KDIR)" || { echo "Set KDIR." >&2; exit 1; }
	$(MAKE) -C "$(KDIR)" M="$(PWD)" DNSK_XT_NOCOMPAT="$(DNSK_XT_NOCOMPAT)" modules
modules: all
clean:
	$(MAKE) -C "$(KDIR)" M="$(PWD)" clean
