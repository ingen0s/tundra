/*
 * Tundra Chat Protocol, version 1.
 *
 * Every frame is a 7-byte header followed by `length` payload bytes.
 * All multi-byte integers are big-endian (network order).
 *
 *   offset 0  version   u8   always 0x01
 *   offset 1  flags     u8   reserved, must be 0
 *   offset 2  seq       u16  sequence number
 *   offset 4  length    u16  number of payload bytes AFTER the 7-byte header
 *   offset 6  type      u8   message type
 *   offset 7  payload   length bytes
 *
 * Total frame size is always 7 + length.
 */
#ifndef TUNDRA_PROTOCOL_H
#define TUNDRA_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define TUNDRA_VERSION      0x01
#define TUNDRA_HEADER_LEN   7
#define TUNDRA_MAX_PAYLOAD  65535
#define TUNDRA_MAX_FRAME    (TUNDRA_HEADER_LEN + TUNDRA_MAX_PAYLOAD)
#define TUNDRA_MAX_USERNAME 32
#define TUNDRA_UUID_LEN     16

/* Longest chat text that still fits in a broadcast frame:
 * broadcast payload = ulen(1) + username + timestamp(8) + mlen(2) + text */
#define TUNDRA_MAX_TEXT (TUNDRA_MAX_PAYLOAD - 11 - TUNDRA_MAX_USERNAME)

enum TundraMessageType {
    /* client -> server */
    MSG_REGISTER          = 0x01, /* id(16) ulen(1) username(ulen)                    */
    MSG_MESSAGE           = 0x02, /* mlen(2) text(mlen)                               */
    MSG_AGENT_COMMAND     = 0x03, /* alen(1) agent(alen) clen(2) command(clen)        */
    MSG_PONG              = 0x04, /* echo of the PING payload (optional)              */
    MSG_HEALTH_CHECK      = 0x05, /* empty                                            */

    /* server -> client */
    MSG_SYSTEM            = 0x10, /* id(16) mlen(2) text(mlen); id is nil for notices */
    MSG_BROADCAST_MESSAGE = 0x11, /* ulen(1) username ts_ms(8) mlen(2) text           */
    MSG_BROADCAST_AGENT   = 0x12, /* same layout as BROADCAST_MESSAGE, text = "agent command" */
    MSG_IMAGE             = 0x13, /* reserved, not implemented                        */
    MSG_PING              = 0x14, /* ts_ms(8)                                         */
    MSG_ERROR             = 0x15, /* code(2) mlen(2) text(mlen)                       */
    MSG_HEALTH_RESULT     = 0x16  /* ts_ms(8) status(1) latency_ms(4) uptime_ms(4)
                                     sent(4) received(4)                              */
};

enum TundraErrorCode {
    ERR_USERNAME_TAKEN  = 0x0001,
    ERR_INVALID_FORMAT  = 0x0002,
    ERR_SESSION_EXPIRED = 0x0003,
    ERR_SERVER_ERROR    = 0x0004,
    ERR_NOT_REGISTERED  = 0x0005
};

/* ---- big-endian helpers ---- */

static inline uint16_t tundra_get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint64_t tundra_get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static inline void tundra_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void tundra_put_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> ((3 - i) * 8));
}

static inline void tundra_put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> ((7 - i) * 8));
}

/* Writes a header for a payload of `payload_len` bytes. Caller guarantees
 * payload_len <= TUNDRA_MAX_PAYLOAD. */
static inline void tundra_put_header(uint8_t *hdr, uint8_t type, uint16_t seq, size_t payload_len) {
    hdr[0] = TUNDRA_VERSION;
    hdr[1] = 0x00;
    tundra_put_u16(hdr + 2, seq);
    tundra_put_u16(hdr + 4, (uint16_t)payload_len);
    hdr[6] = type;
}

/* ---- bounds-checked payload reader ----
 * Every read checks remaining length first. Once a read fails, `ok` stays 0
 * and all later reads return zero/NULL, so callers check `ok` once at the end. */
typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    int ok;
} TundraReader;

static inline TundraReader tundra_reader(const uint8_t *p, size_t len) {
    TundraReader r = { p, len, 0, 1 };
    return r;
}

static inline uint8_t tundra_rd_u8(TundraReader *r) {
    if (!r->ok || r->len - r->pos < 1) { r->ok = 0; return 0; }
    return r->p[r->pos++];
}

static inline uint16_t tundra_rd_u16(TundraReader *r) {
    if (!r->ok || r->len - r->pos < 2) { r->ok = 0; return 0; }
    uint16_t v = tundra_get_u16(r->p + r->pos);
    r->pos += 2;
    return v;
}

static inline uint64_t tundra_rd_u64(TundraReader *r) {
    if (!r->ok || r->len - r->pos < 8) { r->ok = 0; return 0; }
    uint64_t v = tundra_get_u64(r->p + r->pos);
    r->pos += 8;
    return v;
}

static inline const uint8_t *tundra_rd_bytes(TundraReader *r, size_t n) {
    if (!r->ok || r->len - r->pos < n) { r->ok = 0; return NULL; }
    const uint8_t *b = r->p + r->pos;
    r->pos += n;
    return b;
}

/* True when every read succeeded and the whole payload was consumed. */
static inline int tundra_rd_done(const TundraReader *r) {
    return r->ok && r->pos == r->len;
}

#endif /* TUNDRA_PROTOCOL_H */
