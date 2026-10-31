# SPDX-License-Identifier: GPL-2.0-only
#
# Core is the only module in M0.  Feature modules are added only after their
# feasibility gate and build as independent .ko files.

obj-m += dnsk_core.o
dnsk_core-y := \
	core/dnsk_core.o \
	core/dnsk_symbols.o \
	core/dnsk_patch.o \
	core/dnsk_hook.o \
	core/dnsk_feature.o \
	core/dnsk_diag.o \
	core/dnsk_log.o \
	core/dnsk_panic.o

ccflags-y += -I$(src) -I$(src)/core -I$(src)/core/include
ccflags-y += -Wno-cast-function-type
