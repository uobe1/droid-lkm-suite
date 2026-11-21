# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host-side build gate for the Droid-Hook library skeleton.
#
# This is NOT a kernel module build.  There is no Kbuild here on purpose: the
# library layer must compile in both __KERNEL__ and host contexts, so each
# translation unit is syntax/type-checked with the host compiler and no kernel
# headers.  The runtime adapter uses a mock of struct dnsk_core_api in its
# harness so core/include/dnsk_core.h is never needed on the host.
#
# Files are compiled individually (never linked together): teammates provide
# their own harness stubs for functions owned by other translation units, so
# linking everything into one binary would mix incompatible stubs.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror
INCS    := -Iinclude -Iruntime
BUILDD  := build/host
SRCS    := $(sort $(wildcard src/*.c) $(wildcard runtime/*.c))
OBJS    := $(patsubst %.c,$(BUILDD)/%.o,$(SRCS))
TESTS   := $(sort $(wildcard tests/host/test_*.py))

.PHONY: all check compile test clean

all: check

check: compile test
	@echo "droid-hook: host gate clean ($(words $(SRCS)) TUs, $(words $(TESTS)) suites)"

compile: $(OBJS)

$(BUILDD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

test:
	@set -e; for t in $(TESTS); do \
		echo "== $$t"; \
		(cd tests/host && python3 -m unittest -v "$$(basename $$t .py)") || exit 1; \
	done

clean:
	rm -rf $(BUILDD)
