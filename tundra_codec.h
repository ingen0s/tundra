/*
 * Structured encode/decode for every Tundra message payload.
 *
 * Decoding never copies: byte fields point into the caller's buffer and are
 * only valid while that buffer is. Decoding is purely structural (lengths,
 * exact payload size); semantic checks such as allowed username characters
 * belong to the caller.
 *
 * The encoding is canonical: for any payload that decodes successfully,
 * re-encoding the result yields the identical bytes. The fuzz harness in
 * fuzz/ checks this property.
 */
#ifndef TUNDRA_CODEC_H
#define TUNDRA_CODEC_H

#include "tundra_protocol.h"

typedef struct {
    const uint8_t *p;
    size_t len;
} TundraBytes;

#define TUNDRA_DECODE_OK        0
#define TUNDRA_DECODE_MALFORMED (-1)
#define TUNDRA_DECODE_UNKNOWN   (-2)

typedef struct {
    uint8_t type;
    union {
        struct { uint8_t id[TUNDRA_UUID_LEN]; TundraBytes username; } reg;          /* REGISTER */
        struct { TundraBytes text; } message;                                       /* MESSAGE */
        struct { TundraBytes agent; TundraBytes command; } agent_cmd;               /* AGENT_COMMAND */
        struct { TundraBytes data; } pong;                                          /* PONG (opaque) */
        struct { uint8_t id[TUNDRA_UUID_LEN]; TundraBytes text; } system;           /* SYSTEM */
        struct { TundraBytes sender; uint64_t ts_ms; TundraBytes text; } broadcast; /* BROADCAST_* */
        struct { uint64_t ts_ms; } ping;                                            /* PING */
        struct { uint16_t code; TundraBytes text; } error;                          /* ERROR */
        struct {
            uint64_t ts_ms;
            uint8_t status;
            uint32_t latency_ms, uptime_ms, sent, received;
        } health;                                                                   /* HEALTH_RESULT */
    } u;
} TundraMsg;

/* Decodes `len` payload bytes of message `type` into `out`.
 * Returns TUNDRA_DECODE_OK, _MALFORMED or _UNKNOWN. */
int tundra_decode(uint8_t type, const uint8_t *payload, size_t len, TundraMsg *out);

/* Encodes `msg` into `buf`. Returns the payload length, or -1 when the
 * message does not fit, a field exceeds its length prefix, or the type is
 * unknown. */
long tundra_encode(const TundraMsg *msg, uint8_t *buf, size_t cap);

/* Human-readable type name, for logs. */
const char *tundra_type_name(uint8_t type);

#endif /* TUNDRA_CODEC_H */
