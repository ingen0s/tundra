CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -Werror
LDLIBS  += -luuid

BUILD   := build
SERVER  := $(BUILD)/tundra_chat_server
CLIENT  := $(BUILD)/tundra_client
CODEC_T := $(BUILD)/codec_test
FUZZER  := $(BUILD)/fuzz_codec
HEADERS := tundra_protocol.h tundra_codec.h
CODEC   := tundra_codec.c
SERVER_SRC := $(wildcard server/*.c)

FUZZ_TIME ?= 60

.PHONY: all clean test asan fuzz fuzz-run

all: $(SERVER) $(CLIENT) $(CODEC_T)

$(BUILD):
	mkdir -p $(BUILD)

$(SERVER): $(SERVER_SRC) server/server.h $(CODEC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC) $(CODEC) $(LDLIBS)

$(CLIENT): test_client.c $(CODEC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_client.c $(CODEC) $(LDLIBS)

$(CODEC_T): tests/codec_test.c $(CODEC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/codec_test.c $(CODEC)

# Build with AddressSanitizer and UndefinedBehaviorSanitizer.
asan:
	$(MAKE) clean
	$(MAKE) all CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"

test: all
	./$(CODEC_T)
	./tests/integration.sh

# libFuzzer target (needs clang).
fuzz: | $(BUILD)
	clang -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -o $(FUZZER) fuzz/fuzz_codec.c $(CODEC)

# Fuzz for FUZZ_TIME seconds; new interesting inputs go to build/corpus,
# the checked-in seeds in fuzz/corpus are only read.
fuzz-run: fuzz
	mkdir -p $(BUILD)/corpus
	./$(FUZZER) $(BUILD)/corpus fuzz/corpus -max_total_time=$(FUZZ_TIME) -print_final_stats=1

clean:
	rm -rf $(BUILD)
