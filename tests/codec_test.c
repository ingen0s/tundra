/* Unit tests for tundra_codec. Run via `make test`. */
#include <stdio.h>
#include <string.h>

#include "../tundra_codec.h"

static int failures = 0;

#define CHECK(cond, name)                                         \
    do {                                                          \
        if (cond) {                                               \
            printf("ok   - %s\n", name);                          \
        } else {                                                  \
            printf("FAIL - %s (%s:%d)\n", name, __FILE__, __LINE__); \
            failures++;                                           \
        }                                                         \
    } while (0)

static TundraBytes str(const char *s) {
    TundraBytes b = { (const uint8_t *)s, strlen(s) };
    return b;
}

static int bytes_eq(TundraBytes b, const char *s) {
    return b.len == strlen(s) && memcmp(b.p, s, b.len) == 0;
}

int main(void) {
    uint8_t buf[TUNDRA_MAX_PAYLOAD];
    TundraMsg in, out;

    /* REGISTER round trip */
    memset(&in, 0, sizeof(in));
    in.type = MSG_REGISTER;
    memset(in.u.reg.id, 0xab, TUNDRA_UUID_LEN);
    in.u.reg.username = str("alice");
    long n = tundra_encode(&in, buf, sizeof(buf));
    CHECK(n == 16 + 1 + 5, "register encodes to id + len + name");
    CHECK(tundra_decode(MSG_REGISTER, buf, (size_t)n, &out) == TUNDRA_DECODE_OK, "register decodes");
    CHECK(bytes_eq(out.u.reg.username, "alice") && out.u.reg.id[15] == 0xab, "register fields survive");

    /* truncated and trailing bytes are rejected */
    CHECK(tundra_decode(MSG_REGISTER, buf, (size_t)n - 1, &out) == TUNDRA_DECODE_MALFORMED, "truncated register rejected");
    buf[n] = 0;
    CHECK(tundra_decode(MSG_REGISTER, buf, (size_t)n + 1, &out) == TUNDRA_DECODE_MALFORMED, "trailing byte rejected");

    /* length prefix larger than the payload */
    const uint8_t bad_msg[] = { 0x00, 0x10, 'h', 'i' };
    CHECK(tundra_decode(MSG_MESSAGE, bad_msg, sizeof(bad_msg), &out) == TUNDRA_DECODE_MALFORMED, "overlong length prefix rejected");

    /* high-bit length bytes are read as unsigned */
    static uint8_t big[2 + 0x8001];
    big[0] = 0x80;
    big[1] = 0x01;
    CHECK(tundra_decode(MSG_MESSAGE, big, sizeof(big), &out) == TUNDRA_DECODE_OK && out.u.message.text.len == 0x8001,
          "length >= 0x8000 decodes as unsigned");

    /* AGENT_COMMAND with agent length pointing past the end */
    const uint8_t bad_agent[] = { 0xff, 'x' };
    CHECK(tundra_decode(MSG_AGENT_COMMAND, bad_agent, sizeof(bad_agent), &out) == TUNDRA_DECODE_MALFORMED, "agent length past end rejected");

    /* HEALTH_RESULT is exactly 25 bytes */
    memset(&in, 0, sizeof(in));
    in.type = MSG_HEALTH_RESULT;
    in.u.health.ts_ms = 1234567890123ULL;
    in.u.health.status = 1;
    in.u.health.latency_ms = 42;
    in.u.health.received = 7;
    n = tundra_encode(&in, buf, sizeof(buf));
    CHECK(n == 25, "health result is 25 bytes");
    CHECK(tundra_decode(MSG_HEALTH_RESULT, buf, 25, &out) == TUNDRA_DECODE_OK && out.u.health.latency_ms == 42 &&
          out.u.health.ts_ms == 1234567890123ULL && out.u.health.received == 7, "health result fields survive");

    /* BROADCAST layout matches the documented wire format byte for byte */
    memset(&in, 0, sizeof(in));
    in.type = MSG_BROADCAST_MESSAGE;
    in.u.broadcast.sender = str("bo");
    in.u.broadcast.ts_ms = 0x0102030405060708ULL;
    in.u.broadcast.text = str("hey");
    n = tundra_encode(&in, buf, sizeof(buf));
    const uint8_t expect[] = { 2, 'b', 'o', 1, 2, 3, 4, 5, 6, 7, 8, 0, 3, 'h', 'e', 'y' };
    CHECK(n == (long)sizeof(expect) && memcmp(buf, expect, sizeof(expect)) == 0, "broadcast wire layout");

    /* encoder refuses fields that do not fit their prefix or the buffer */
    memset(&in, 0, sizeof(in));
    in.type = MSG_REGISTER;
    static char longname[300];
    memset(longname, 'a', sizeof(longname) - 1);
    in.u.reg.username = str(longname);
    CHECK(tundra_encode(&in, buf, sizeof(buf)) == -1, "str8 longer than 255 refused");
    in.u.reg.username = str("alice");
    CHECK(tundra_encode(&in, buf, 10) == -1, "small output buffer refused");

    CHECK(tundra_decode(0x7f, buf, 0, &out) == TUNDRA_DECODE_UNKNOWN, "unknown type reported");

    if (failures) {
        printf("%d codec test(s) failed\n", failures);
        return 1;
    }
    printf("all codec tests passed\n");
    return 0;
}
