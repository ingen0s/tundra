CC      ?= cc
CFLAGS  ?= -O2 -g
# Required flags are appended with `override` so `make CFLAGS=...` cannot drop them.
override CFLAGS += -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -Werror $(SANFLAGS)
SANITIZE := -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
LDLIBS  += -luuid

BUILD   := build
SERVER  := $(BUILD)/tundra_chat_server
CLIENT  := $(BUILD)/tundra_client
HEADERS := tundra_protocol.h

.PHONY: all clean test asan

all: $(SERVER) $(CLIENT)

$(BUILD):
	mkdir -p $(BUILD)

$(SERVER): tundra_chat_server.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(CLIENT): test_client.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Build with AddressSanitizer and UndefinedBehaviorSanitizer.
asan:
	$(MAKE) clean
	$(MAKE) all CFLAGS="-O1 -g" SANFLAGS="$(SANITIZE)"

test: all
	./tests/integration.sh

clean:
	rm -rf $(BUILD)
