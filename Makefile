CC      ?= gcc
CFLAGS  ?= -O3 -std=c99 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lpthread

THREADS ?= 32
OPS     ?= 50000
ZONES   ?= 4

SRC = fun_idpool.c test_idpool.c
HDR = fun_idpool.h arch_defs.h
BIN = test_idpool

# ---- 架构自适应 ----
ARCH := $(shell uname -m)
ARCHFILE := $(shell uname -m)

# 默认: 无额外 flags
EXTRA_CFLAGS :=
EXTRA_LDFLAGS :=

# x86_64
ifeq ($(ARCH),x86_64)
    EXTRA_CFLAGS := -march=native
endif

# x86_32 (i386 or i686)
ifneq ($(filter i386 i686,$(ARCH)),)
    EXTRA_CFLAGS := -m32 -march=i586
    EXTRA_LDFLAGS := -latomic
endif

# AArch64 (ARM 64-bit)
ifeq ($(ARCH),aarch64)
    EXTRA_CFLAGS := -march=armv8-a+lse
endif

# ARMv7 (32-bit) — 排除 aarch64
ifneq ($(filter armv7l armv6l,$(ARCH)),)
    EXTRA_CFLAGS := -march=armv7-a -mfpu=neon
    EXTRA_LDFLAGS := -latomic
endif

# LoongArch64 v1.1 (LA664, 3A6000) — 用户手动指定
ifeq ($(ARCH),loongarch64_la664)
    EXTRA_CFLAGS := -march=la664 -mtune=la664
endif

# LoongArch64 v1.0 (LA464, 3A5000/3C5000)
ifeq ($(ARCH),loongarch64)
    EXTRA_CFLAGS := -march=la464 -mtune=la464
    EXTRA_LDFLAGS := -latomic
endif

# 合并
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

run: $(BIN)
	./$(BIN) $(THREADS) $(OPS) $(ZONES)

debug: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -O0 -g $(SRC) -o $(BIN)_dbg $(LDFLAGS)
	./$(BIN)_dbg $(THREADS) $(OPS) $(ZONES)

asan: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -fsanitize=address -g $(SRC) -o $(BIN)_asan $(LDFLAGS)
	./$(BIN)_asan $(THREADS) $(OPS) $(ZONES)

info:
	@echo "Architecture: $(ARCH)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"

clean:
	rm -f $(BIN) $(BIN)_dbg $(BIN)_asan

.PHONY: all run debug asan info clean
