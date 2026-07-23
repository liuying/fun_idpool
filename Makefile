# fun_idpool — 跨平台无锁 ID 池 Makefile

CC      ?= gcc
CFLAGS  ?= -O3 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-overlength-strings
LDFLAGS ?=

# ---- 架构自适应 ----
# 检测当前架构,为 CFLAGS/LDFLAGS 添加对应的 CPU 指令集与 libatomic 依赖
ARCH := $(shell uname -m 2>/dev/null || echo unknown)

# 龙芯 / ARM / x86 自动适配
# loongarch64: 默认按 la664 (3A6000) 编;老 LA464 (3A5000) 需手动改 -march=la464
# aarch64:    启用 LSE 原子指令(LDADD/STADD 等)
# x86_64:     用 -march=native 启用 AVX2/BMI 等指令
# i386:       32 位编译,需 -latomic 软实现 64 位原子
# armv7l:     ARMv7 + NEON,需 -latomic 软实现 64 位原子
ifeq ($(ARCH),loongarch64)
    CFLAGS  += -march=la664
    LDFLAGS +=
else ifeq ($(ARCH),aarch64)
    CFLAGS  += -march=armv8-a+lse
else ifeq ($(ARCH),x86_64)
    CFLAGS  += -march=native
else ifeq ($(ARCH),i386)
    CFLAGS  += -m32 -march=i586
    LDFLAGS += -latomic
else ifeq ($(ARCH),armv7l)
    CFLAGS  += -march=armv7-a -mfpu=neon
    LDFLAGS += -latomic
endif

# ---- 文件 ----
SRC_C    := fun_idpool.c
SRC_TEST := test_split.c
HDR      := fun_idpool.h arch_defs.h
BIN      := test_split
BIN_DBG  := test_split_dbg
BIN_ASAN := test_split_asan

# ---- 默认目标 ----
all: $(BIN)

$(BIN): $(SRC_C) $(SRC_TEST) $(HDR)
	$(CC) $(CFLAGS) $(SRC_C) $(SRC_TEST) -o $@ $(LDFLAGS) -lpthread

# ---- Debug 构建 (-O0, 保留所有符号) ----
debug: $(BIN_DBG)
$(BIN_DBG): $(SRC_C) $(SRC_TEST) $(HDR)
	$(CC) -O0 -g -std=c99 -Wall -Wextra $(SRC_C) $(SRC_TEST) -o $@ $(LDFLAGS) -lpthread

# ---- AddressSanitizer (-O1 减少 asan 报告噪音) ----
asan: $(BIN_ASAN)
$(BIN_ASAN): $(SRC_C) $(SRC_TEST) $(HDR)
	$(CC) -O1 -g -fsanitize=address -fno-omit-frame-pointer -std=c99 -Wall $(SRC_C) $(SRC_TEST) -o $@ $(LDFLAGS) -lpthread

# ---- 运行 (THREADS / OPS 默认值由测试程序设定) ----
run: $(BIN)
	./$(BIN) $(THREADS) $(OPS)

# ---- 显示构建信息 ----
info:
	@echo "Arch:    $(ARCH)"
	@echo "CC:      $(CC)"
	@echo "CFLAGS:  $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "Files:   $(SRC_C) $(SRC_TEST) $(HDR)"

# ---- 清理构建产物 ----
clean:
	rm -f $(BIN) $(BIN_DBG) $(BIN_ASAN) *.o

.PHONY: all debug asan run info clean
