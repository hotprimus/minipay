CC       ?= cc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes \
            -Wmissing-prototypes -Wconversion -Wsign-conversion -Wundef -Wcast-qual \
            -Wwrite-strings -Wnull-dereference
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS   ?= -O2 -g
LDFLAGS  ?=
LDLIBS   := -pthread

BUILD ?= build
BIN   ?= bin

LIB_SRCS := src/common.c src/log.c src/luhn.c src/iso8583.c src/accounts.c \
            src/journal.c src/processor.c src/net.c src/threadpool.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)

SERVER := $(BIN)/minipay-server
CLIENT := $(BIN)/minipay-cli
TESTS  := $(BIN)/minipay-tests

ALL_CFLAGS = $(CSTD) $(WARN) $(CFLAGS) -pthread -MMD -MP

.PHONY: all test asan tsan valgrind demo clean

all: $(SERVER) $(CLIENT)

$(SERVER): $(BUILD)/server_main.o $(LIB_OBJS) | $(BIN)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(CLIENT): $(BUILD)/client_main.o $(LIB_OBJS) | $(BIN)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TESTS): $(BUILD)/test_main.o $(LIB_OBJS) | $(BIN)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c -o $@ $<

$(BUILD)/test_main.o: tests/test_main.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c -o $@ $<

$(BUILD) $(BIN):
	mkdir -p $@

test: $(TESTS)
	./$(TESTS)

SAN_FLAGS := -O1 -g -fno-omit-frame-pointer

asan:
	$(MAKE) BUILD=build/asan BIN=bin/asan \
	    CFLAGS="$(SAN_FLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all" \
	    LDFLAGS="-fsanitize=address,undefined" test

tsan:
	$(MAKE) BUILD=build/tsan BIN=bin/tsan \
	    CFLAGS="$(SAN_FLAGS) -fsanitize=thread" LDFLAGS="-fsanitize=thread" test

valgrind: $(TESTS)
	valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
	    --error-exitcode=1 ./$(TESTS)

demo: all
	./scripts/demo.sh

clean:
	rm -rf build bin

-include $(LIB_OBJS:.o=.d) $(BUILD)/server_main.d $(BUILD)/client_main.d $(BUILD)/test_main.d
