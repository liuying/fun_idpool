# fun_idpool — 拆分结构体版 Makefile

CC      ?= gcc
CFLAGS  ?= -O3 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-overlength-strings
LDFLAGS ?=

# 架构检测
ARCH := $(shell uname -m 2>/dev/null || echo unknown)

# 龙芯 / ARM / x86 自动适配
ifeq ($(ARCH),loongarch64)
    CFLAGS += -march=la664
    LDFLAGS +=
else ifeq ($(ARCH),aarch64)
    CFLAGS += -march=armv8-a+lse
else ifeq ($(ARCH),x86_64)
    CFLAGS += -march=native
else ifeq ($(ARCH),i386)
    CFLAGS += -m32 -march=i586
    LDFLAGS += -latomic
else ifeq ($(ARCH),armv7l)
    CFLAGS += -march=armv7-a -mfpu=neon
    LDFLAGS += -latomic
endif

# 文件
SRC_C   := fun_idpool.c
SRC_TEST := test_split.c
HDR     := fun_idpool.h arch_defs.h
BIN     := test_split
BIN_DBG := test_split_dbg
BIN_ASAN := test_split_asan

# 默认目标
all: $(BIN)

$(BIN): $(SRC_C) $(SRC_TEST) $(HDR)
	$(CC) $(CFLAGS) $(SRC_C) $(SRC_TEST) -o $@ $(LDFLAGS) -lpthread

# Debug 构建
debug: $(BIN_DBG)
$(BIN_DBG): $(SRC_C) $(SRC_TEST) $(HDR)
	$(CC) -O0 -g -std=c99 -Wall -Wextra $(SRC_C) $(SRC_TEST) -o $@ $(LDFLAGS) -lpthread

# AddressSanitizer
asan: $(BIN_ASAN)
$(BIN_ASAN): $(SRC_C) $(SRC_TEST) $(HDR)
	$(CC) -O1 -g -fsanitize=address -fno-omit-frame-pointer -std=c99 -Wall $(SRC_C) $(SRC_TEST) -o $@ $(LDFLAGS) -lpthread

# 运行
run: $(BIN)
	./$(BIN) $(THREADS) $(OPS)

# 显示构建信息
info:
	@echo "Arch:    $(ARCH)"
	@echo "CC:      $(CC)"
	@echo "CFLAGS:  $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "Files:   $(SRC_C) $(SRC_TEST) $(HDR)"

# 清理
clean:
	rm -f $(BIN) $(BIN_DBG) $(BIN_ASAN) *.o

.PHONY: all debug asan run info clean
