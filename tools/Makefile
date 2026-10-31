# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-compiled static arm64 helpers for the KSU module.

CC ?= clang
TARGET ?= aarch64-linux-android24
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
BIN := bin

.PHONY: all clean

all: $(BIN)/dnsk-preflight

$(BIN)/dnsk-preflight: dnsk_preflight.c | $(BIN)
	$(CC) --target=$(TARGET) $(CFLAGS) -static -o $@ $<

$(BIN):
	mkdir -p $(BIN)

clean:
	rm -rf $(BIN)
