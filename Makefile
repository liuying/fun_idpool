CC      ?= gcc
CFLAGS  ?= -O3 -std=c99 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lpthread

SRC = fun_idpool.c test_v9.c
HDR = fun_idpool.h arch_defs.h
BIN = test_v9

.PHONY: all run debug asan clean info

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

info:
	@echo "ARCH=$(shell uname -m)"
	@echo "CC=$(CC)"
	@echo "CFLAGS=$(CFLAGS)"

clean:
	rm -f $(BIN)
