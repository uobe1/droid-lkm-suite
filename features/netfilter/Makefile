# SPDX-License-Identifier: GPL-2.0-only
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all modules clean
all:
	@test -n "$(KDIR)" || { echo "Set KDIR." >&2; exit 1; }
	$(MAKE) -C "$(KDIR)" M="$(PWD)" DNSK_XT_NOCOMPAT="$(DNSK_XT_NOCOMPAT)" \
		DNSK_XT_LAYOUT="$(DNSK_XT_LAYOUT)" DNSK_ABI_PROFILE="$(DNSK_ABI_PROFILE)" \
		DNSK_PAGE_SIZE="$(DNSK_PAGE_SIZE)" DNSK_MAX_SKB_FRAGS="$(DNSK_MAX_SKB_FRAGS)" \
		DNSK_CGROUP_SUBSYS_COUNT="$(DNSK_CGROUP_SUBSYS_COUNT)" modules
modules: all
clean:
	$(MAKE) -C "$(KDIR)" M="$(PWD)" clean
