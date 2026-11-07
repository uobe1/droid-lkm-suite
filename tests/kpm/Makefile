# Build a KPM against the KernelPatch 0.13.9 kernel headers.
#   make KP_DIR=/path/to/KernelPatch TARGET_COMPILE=aarch64-linux-gnu-
# Include layout and `-r` linking follow kpms/demo-syscallhook/Makefile at tag
# 0.13.9.  A distro aarch64-linux-gnu-gcc may be built --enable-default-pie
# and/or with a default stack protector, while KernelPatch 0.13.9
# kernel/patch/module/relo.c only accepts non-GOT relocations, so force
# non-PIC code and no stack protector explicitly.
ifndef TARGET_COMPILE
$(error TARGET_COMPILE not set)
endif
ifndef KP_DIR
$(error KP_DIR not set)
endif

CC = $(TARGET_COMPILE)gcc
INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include
INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))
objs := hook_bench.o

all: hook-bench.kpm

hook-bench.kpm: $(objs)
	$(CC) -r -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDE_FLAGS) -c -O2 -fno-pie -fno-pic -fno-stack-protector \
		-mno-outline-atomics -o $@ $<

clean:
	rm -f *.kpm *.o

.PHONY: all clean
