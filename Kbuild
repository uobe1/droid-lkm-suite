# SPDX-License-Identifier: GPL-2.0-only
#
# The repository root builds core only.  Each feature under features/<name>/
# has its own Kbuild and builds an independent .ko.
#
# dnsk_core.ko embeds the droid-hook library by path (no source copy, no
# submodule).  DNSK_HOOK_DIR may be absolute or relative to this module
# directory; the default is the sibling checkout ../droid-hook.  A missing
# checkout is a hard error so `make` never silently builds without it.
DNSK_HOOK_DIR ?= $(src)/../droid-hook
ifeq ($(filter /%,$(DNSK_HOOK_DIR)),)
DNSK_HOOK_PATH := $(src)/$(DNSK_HOOK_DIR)
else
DNSK_HOOK_PATH := $(DNSK_HOOK_DIR)
endif
ifeq ($(wildcard $(DNSK_HOOK_PATH)/src/dh_hook.c),)
$(error dnsk_core: droid-hook not found at DNSK_HOOK_DIR=$(DNSK_HOOK_DIR); expected $(DNSK_HOOK_PATH)/src/dh_hook.c. Set DNSK_HOOK_DIR to a droid-hook checkout)
endif

obj-m += dnsk_core.o
dnsk_core-y := \
	core/dnsk_core.o \
	core/dnsk_symbols.o \
	core/dnsk_patch.o \
	core/dnsk_hook_api.o \
	core/dnsk_feature.o \
	core/dnsk_diag.o \
	core/dnsk_rw.o \
	core/dnsk_log.o \
	core/dnsk_panic.o \
	core/dnsk_dh_rw.o \
	core/dnsk_dh_txn.o \
	core/dnsk_dh_insn.o \
	core/dnsk_dh_hook.o \
	core/dnsk_dh_shadow.o \
	core/dnsk_dh_preflight.o

ccflags-y += -I$(src) -I$(src)/core -I$(src)/core/include
ccflags-y += -I$(DNSK_HOOK_PATH)/include
ccflags-y += -DDNSK_DH_RW_SRC='"$(DNSK_HOOK_PATH)/src/dh_rw.c"'
ccflags-y += -DDNSK_DH_TXN_SRC='"$(DNSK_HOOK_PATH)/src/dh_txn.c"'
ccflags-y += -DDNSK_DH_INSN_SRC='"$(DNSK_HOOK_PATH)/src/dh_insn.c"'
ccflags-y += -DDNSK_DH_HOOK_SRC='"$(DNSK_HOOK_PATH)/src/dh_hook.c"'
ccflags-y += -DDNSK_DH_SHADOW_SRC='"$(DNSK_HOOK_PATH)/src/dh_shadow.c"'
ccflags-y += -DDNSK_DH_PREFLIGHT_SRC='"$(DNSK_HOOK_PATH)/src/dh_preflight.c"'
