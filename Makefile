CC      ?= cc
CFLAGS  ?= -O2 -g
# Required flags are appended with `override` so `make CFLAGS=...` cannot drop them.
override CFLAGS += -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -Werror $(SANFLAGS)
SANITIZE := -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
# TLS: on when OpenSSL is found (pkg-config), off with TLS=0.
TLS ?= auto
ifeq ($(TLS),auto)
  override TLS := $(shell pkg-config --exists openssl 2>/dev/null && echo 1 || echo 0)
endif
ifeq ($(TLS),1)
  TLS_CFLAGS := -DTUNDRA_TLS $(shell pkg-config --cflags openssl 2>/dev/null)
  TLS_LIBS   := $(or $(shell pkg-config --libs openssl 2>/dev/null),-lssl -lcrypto)
endif

# Per-platform settings. Windows builds use MinGW-w64 (MSYS2).
ifeq ($(OS),Windows_NT)
  EXE      := .exe
  NET_LIBS := -lws2_32 -lbcrypt
  override CFLAGS += -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32_WINNT=0x0600
else
  EXE      :=
  NET_LIBS :=
endif

BUILD   := build
SERVER  := $(BUILD)/tundra_chat_server$(EXE)
CLIENT  := $(BUILD)/tundra_client$(EXE)
CODEC_T := $(BUILD)/codec_test$(EXE)
FUZZER  := $(BUILD)/fuzz_codec
HEADERS := tundra_protocol.h tundra_codec.h
CODEC   := tundra_codec.c
PLATFORM := tundra_platform.c tundra_platform.h
SERVER_SRC := $(wildcard server/*.c)

FUZZ_TIME ?= 60
FUZZ_CC   ?= $(shell command -v clang-18 || echo clang)

.PHONY: all clean test asan fuzz fuzz-run

all: $(SERVER) $(CLIENT) $(CODEC_T)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/web_index.h: web/index.html tools/embed.py | $(BUILD)
	python3 tools/embed.py web/index.html $@ web_index_html

$(SERVER): $(SERVER_SRC) server/server.h $(CODEC) $(PLATFORM) $(HEADERS) $(BUILD)/web_index.h | $(BUILD)
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -I$(BUILD) -o $@ $(SERVER_SRC) $(CODEC) tundra_platform.c $(LDFLAGS) -lsqlite3 $(TLS_LIBS) $(LDLIBS) $(NET_LIBS)

$(CLIENT): test_client.c $(CODEC) $(PLATFORM) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -o $@ test_client.c $(CODEC) tundra_platform.c $(LDFLAGS) $(TLS_LIBS) $(LDLIBS) $(NET_LIBS)

$(CODEC_T): tests/codec_test.c $(CODEC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/codec_test.c $(CODEC)

# Build with AddressSanitizer and UndefinedBehaviorSanitizer.
asan:
	$(MAKE) clean
	$(MAKE) all CFLAGS="-O1 -g" SANFLAGS="$(SANITIZE)"

test: all
	./$(CODEC_T)
	./tests/integration.sh

# libFuzzer target (needs clang).
fuzz: | $(BUILD)
	$(FUZZ_CC) -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -o $(FUZZER) fuzz/fuzz_codec.c $(CODEC)

# Fuzz for FUZZ_TIME seconds; new interesting inputs go to build/corpus,
# the checked-in seeds in fuzz/corpus are only read.
fuzz-run: fuzz
	mkdir -p $(BUILD)/corpus
	./$(FUZZER) $(BUILD)/corpus fuzz/corpus -max_total_time=$(FUZZ_TIME) -max_len=65536 -print_final_stats=1

clean:
	rm -rf $(BUILD)
