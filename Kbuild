# SPDX-License-Identifier: GPL-2.0-only
#
# The repository root builds core only.  Each feature under features/<name>/
# has its own Kbuild and builds an independent .ko.

obj-m += dnsk_core.o
dnsk_core-y := \
	core/dnsk_core.o \
	core/dnsk_symbols.o \
	core/dnsk_patch.o \
	core/dnsk_hook.o \
	core/dnsk_feature.o \
	core/dnsk_diag.o \
	core/dnsk_rw.o \
	core/dnsk_log.o \
	core/dnsk_panic.o

ccflags-y += -I$(src) -I$(src)/core -I$(src)/core/include
