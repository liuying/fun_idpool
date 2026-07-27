CC      ?= gcc
CFLAGS  ?= -O3 -std=c99 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lpthread

SRC = fun_idpool.c test_v9.c
HDR = fun_idpool.h arch_defs.h
BIN = test_v9

.PHONY: all run debug asan tsan clean info

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

debug: CFLAGS = -O0 -g -std=c99 -Wall -Wextra
debug: clean $(BIN)
	./$(BIN)

asan: CFLAGS = -O1 -g -std=c99 -Wall -Wextra -fsanitize=address -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address
asan: clean $(BIN)
	./$(BIN)

# TSAN 需要clang (gcc 13 在当前 glibc 有 TSAN runtime bug)
# 用法: make tsan CC=clang
tsan: CC = clang
tsan: CFLAGS = -O1 -g -std=c99 -Wall -Wextra -Wno-unused-parameter -fsanitize=thread -fno-omit-frame-pointer
tsan: LDFLAGS += -fsanitize=thread
tsan: clean $(BIN)
	TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" ./$(BIN)

info:
	@echo "ARCH=$(shell uname -m)"
	@echo "CC=$(CC)"
	@echo "CFLAGS=$(CFLAGS)"

clean:
	rm -f $(BIN)
