/*
 * libFuzzer target for the payload codec.
 *
 * Input layout: first byte is the message type, the rest is the payload.
 * Checks that decoding never misbehaves and that every payload that decodes
 * successfully re-encodes to exactly the same bytes.
 *
 *   make fuzz && ./build/fuzz_codec fuzz/corpus -max_total_time=60
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tundra_codec.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size - 1 > TUNDRA_MAX_PAYLOAD) return 0;
    uint8_t type = data[0];
    const uint8_t *payload = data + 1;
    size_t len = size - 1;

    /* copy into an exact-size heap buffer so ASan catches any over-read */
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return 0;
    memcpy(copy, payload, len);

    TundraMsg msg;
    int rc = tundra_decode(type, copy, len, &msg);
    if (rc == TUNDRA_DECODE_OK) {
        static uint8_t out[TUNDRA_MAX_PAYLOAD];
        long n = tundra_encode(&msg, out, sizeof(out));
        if (n < 0 || (size_t)n != len || memcmp(out, copy, len) != 0) {
            fprintf(stderr, "round trip mismatch for type 0x%02x (len %zu, re-encoded %ld)\n", type, len, n);
            abort();
        }
    }
    free(copy);
    return 0;
}
