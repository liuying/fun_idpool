# fun_idpool v8 — Makefile

# ---- 编译参数 ----
# CC:      编译器
# CFLAGS:  Release 编译选项 (-O3 优化, C99 标准, 严格警告)
# LDFLAGS: 链接 pthread
CC      ?= gcc
CFLAGS  ?= -O3 -std=c99 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lpthread

# ---- 文件 ----
# SRC: 源文件 (fun_idpool.c + 测试)
# HDR: 头文件
# BIN: 输出可执行文件
SRC = fun_idpool.c test_v8.c
HDR = fun_idpool.h arch_defs.h
BIN = test_v8

# ---- 目标 ----
.PHONY: all run debug asan clean info

# 默认: Release 编译
all: $(BIN)

# 编译规则
$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

# 运行测试
run: $(BIN)
	./$(BIN)

# Debug 构建: -O0 -g, 保留所有符号
debug: CFLAGS = -O0 -g -std=c99 -Wall -Wextra
debug: clean $(BIN)
	./$(BIN)

# AddressSanitizer: 检测内存错误 (越界/use-after-free/leak)
asan: CFLAGS = -O1 -g -std=c99 -Wall -Wextra -fsanitize=address -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address
asan: clean $(BIN)
	./$(BIN)

# 显示当前构建配置
info:
	@echo "ARCH=$(shell uname -m)"
	@echo "CC=$(CC)"
	@echo "CFLAGS=$(CFLAGS)"

# 清理构建产物
clean:
	rm -f $(BIN)
