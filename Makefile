# fun_idpool — Region #0 bit 0 保留版
# Cross-platform Makefile: x86_64 / x86_32 / AArch64 / ARMv7 / LoongArch64

# ---- 用户可调参数 ----
# CC:     编译器
# ARCH:   目标架构 (auto = uname 自动检测; 也可手动指定如 x86_64/aarch64 等)
# THREADS:run 默认线程数 (传给 test_bit0)
# OPS:    run 默认每线程操作数
# ZONES:  NUMA zone 数
CC      ?= gcc
ARCH    ?= auto
THREADS ?= 16
OPS     ?= 5000
ZONES   ?= 4

# ---- 架构检测 ----
# 通过 uname -m 自动识别当前主机架构, 用于下方条件编译 flags
# 注意: UNAME_M 必须在 ifeq 外用 := 求值, 否则 ifeq 比较时无法展开
UNAME_M := $(shell uname -m 2>/dev/null || echo unknown)
ifeq ($(ARCH),auto)
    ifeq ($(UNAME_M),x86_64)
        ARCH := x86_64
    else ifeq ($(UNAME_M),aarch64)
        ARCH := aarch64
    else ifeq ($(UNAME_M),loongarch64)
        ARCH := loongarch64
    else ifeq ($(UNAME_M),i686)
        ARCH := i386
    else ifneq ($(filter arm%,$(UNAME_M)),)
        ARCH := arm
    else
        ARCH := unknown
    endif
endif

# ---- 编译标志基础集 (所有架构共享) ----
# -std=c99:        严格 C99
# -Wall -Wextra:   开启常规 + 额外警告
# -Wno-unused-parameter: 容忍未使用参数 (pthread 入口函数签名固定)
# -Wno-format:     容忍 printf 格式警告 (自定义格式符)
CFLAGS_BASE := -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-format

# ---- 架构特定 flags ----
# x86_64:   用 -march=native 启用 AVX2/BMI 等指令集
# x86_32:   32 位编译, 需 -latomic 软实现 64 位原子
# aarch64:  启用 LSE 原子指令 (LDADD/STADD 等)
# arm:      ARMv7 + NEON, 需 -latomic 软实现 64 位原子
# loongarch64: 默认按 la664 (3A6000) 编, 老 LA464 (3A5000) 需手动改 -march=la464
ifeq ($(ARCH),x86_64)
    CFLAGS  += $(CFLAGS_BASE) -march=native -O3
    LDFLAGS += -lpthread
else ifeq ($(ARCH),x86_32)
    CFLAGS  += $(CFLAGS_BASE) -m32 -march=i586 -O3
    LDFLAGS += -lpthread -latomic
else ifeq ($(ARCH),aarch64)
    CFLAGS  += $(CFLAGS_BASE) -march=armv8-a+lse -O3
    LDFLAGS += -lpthread
else ifeq ($(ARCH),arm)
    CFLAGS  += $(CFLAGS_BASE) -march=armv7-a -mfpu=neon -O3
    LDFLAGS += -lpthread -latomic
else ifeq ($(ARCH),loongarch64)
    CFLAGS  += $(CFLAGS_BASE) -march=la664 -O3
    LDFLAGS += -lpthread
else
    CFLAGS  += $(CFLAGS_BASE) -O3
    LDFLAGS += -lpthread -latomic
endif

# ---- 文件 ----
HDR    := fun_idpool.h arch_defs.h     # 公共头文件
TARGET := test_bit0                    # 主测试 (6 项综合测试)
VERIFY := verify_bit0                  # 单线程手工验证

# ---- 伪目标 ----
.PHONY: all release debug asan info clean run verify

# ---- 构建目标 ----
# all      = 默认 release 编译
# release  = Release 模式 (继承 CFLAGS_BASE + 架构 flags + -O3)
# debug    = Debug 模式 (-O0 -g -DDEBUG, 保留所有符号)
# asan     = AddressSanitizer (-O1 -g -fsanitize=address, 减少 asan 报告噪音)
all: release

release: CFLAGS := $(CFLAGS)
release: $(TARGET)

debug: CFLAGS := $(CFLAGS_BASE) -O0 -g -DDEBUG
debug: $(TARGET)

asan: CFLAGS := $(CFLAGS_BASE) -O1 -g -fsanitize=address -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address
asan: $(TARGET)

# ---- 编译规则 ----
$(TARGET): fun_idpool.c test_bit0.c $(HDR)
	$(CC) $(CFLAGS) -o $@ fun_idpool.c test_bit0.c $(LDFLAGS)

$(VERIFY): fun_idpool.c verify_bit0.c $(HDR)
	$(CC) $(CFLAGS) -o $@ fun_idpool.c verify_bit0.c $(LDFLAGS)

# ---- 运行目标 ----
# run:    编译并运行主测试 (默认 THREADS × OPS, 可通过命令行覆盖)
# verify: 编译并运行单线程手工验证 (展示 Region #0 bit 0 保留的完整流程)
run: release
	./$(TARGET) $(THREADS) $(OPS)

verify: $(VERIFY)
	./$(VERIFY)

# ---- 信息显示 ----
# info: 打印当前构建配置 (架构 / 编译器 / flags / 文件列表 / 系统信息)
info:
	@echo "=== fun_idpool build info ==="
	@echo "ARCH:    $(ARCH)"
	@echo "CC:      $(CC)"
	@echo "CFLAGS:  $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "TARGET:  $(TARGET)"
	@echo "VERIFY:  $(VERIFY)"
	@uname -a

# ---- 清理 ----
# clean: 删除构建产物 (可执行文件 + .o)
clean:
	rm -f $(TARGET) $(VERIFY) *.o
