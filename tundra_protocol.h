/*
 * Tundra Chat Protocol, version 1.
 *
 * Every frame is a 7-byte header followed by `length` payload bytes.
 * All multi-byte integers are big-endian (network order).
 *
 *   offset 0  version   u8   always 0x01
 *   offset 1  flags     u8   reserved: send 0, ignored by the server
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
#include <string.h>

#define TUNDRA_VERSION      0x01
#define TUNDRA_HEADER_LEN   7
#define TUNDRA_MAX_PAYLOAD  65535
#define TUNDRA_MAX_FRAME    (TUNDRA_HEADER_LEN + TUNDRA_MAX_PAYLOAD)
#define TUNDRA_MAX_USERNAME 32
#define TUNDRA_UUID_LEN     16

#define TUNDRA_MAX_ROOM     32  /* including the leading '#' */
#define TUNDRA_MAX_HISTORY  100 /* entries per HISTORY request */

/* Longest chat text that still fits in any frame that carries it. The
 * tightest is HISTORY_RESULT holding one entry: room(1+32) count(2), then
 * len(2) and a CHAT payload of id(8) ts(8) kind(1) target(1+32)
 * sender(1+32) mlen(2) text. */
#define TUNDRA_MAX_TEXT (TUNDRA_MAX_PAYLOAD - 35 - 2 - 85)

/* Type ranges: 0x01-0x0F and 0x20-0x2F are client -> server,
 * 0x10-0x1F and 0x30-0x3F are server -> client. */
enum TundraMessageType {
    /* client -> server */
    MSG_REGISTER          = 0x01, /* id(16) ulen(1) username(ulen)                    */
    MSG_MESSAGE           = 0x02, /* mlen(2) text: shorthand for SEND to #lobby       */
    MSG_AGENT_COMMAND     = 0x03, /* alen(1) agent clen(2) command                    */
    MSG_PONG              = 0x04, /* echo of the PING payload; keeps the connection alive */
    MSG_HEALTH_CHECK      = 0x05, /* empty                                            */
    MSG_JOIN              = 0x06, /* rlen(1) room                                     */
    MSG_LEAVE             = 0x07, /* rlen(1) room                                     */
    MSG_SEND              = 0x08, /* kind(1) tlen(1) target mlen(2) text              */
    MSG_LIST_ROOMS        = 0x09, /* empty                                            */
    MSG_HISTORY           = 0x0B, /* rlen(1) room before_id(8) limit(2)               */
    MSG_WHO               = 0x0C, /* rlen(1) room                                     */

    /* server -> client */
    MSG_SYSTEM            = 0x10, /* id(16) mlen(2) text(mlen); id is nil for notices */
    MSG_BROADCAST_MESSAGE = 0x11, /* legacy, no longer sent                          */
    MSG_BROADCAST_AGENT   = 0x12, /* ulen(1) username ts_ms(8) mlen(2) text           */
    MSG_IMAGE             = 0x13, /* reserved, not implemented                        */
    MSG_PING              = 0x14, /* ts_ms(8)                                         */
    MSG_ERROR             = 0x15, /* code(2) mlen(2) text(mlen)                       */
    MSG_HEALTH_RESULT     = 0x16, /* ts_ms(8) status(1) latency_ms(4) uptime_ms(4)
                                     sent(4) received(4)                              */
    MSG_CHAT              = 0x17, /* id(8) ts_ms(8) kind(1) tlen(1) target
                                     slen(1) sender mlen(2) text                      */
    MSG_SENT              = 0x18, /* id(8): reply to SEND/MESSAGE                     */
    MSG_ROOM_LIST         = 0x19, /* count(2) then count x [rlen(1) room members(2)]  */
    MSG_ROOM_EVENT        = 0x1A, /* event(1) rlen(1) room ulen(1) user               */
    MSG_HISTORY_RESULT    = 0x1B, /* rlen(1) room count(2) then count x
                                     [len(2) CHAT payload], oldest first              */
    MSG_WHO_RESULT        = 0x1C  /* rlen(1) room count(2) then count x
                                     [ulen(1) user online(1)]                         */
};

enum TundraChatKind {
    CHAT_ROOM   = 0, /* target is a room name */
    CHAT_DIRECT = 1  /* target is the recipient's username */
};

enum TundraRoomEvent {
    ROOM_JOINED = 1,
    ROOM_LEFT   = 2
};

enum TundraErrorCode {
    ERR_USERNAME_TAKEN  = 0x0001,
    ERR_INVALID_FORMAT  = 0x0002,
    ERR_SESSION_EXPIRED = 0x0003,
    ERR_SERVER_ERROR    = 0x0004,
    ERR_NOT_REGISTERED  = 0x0005,
    ERR_NO_SUCH_TARGET  = 0x0006, /* unknown user or room */
    ERR_NOT_A_MEMBER    = 0x0007, /* room action that needs membership */
    ERR_INVALID_NAME    = 0x0008  /* bad room or user name */
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

static inline uint32_t tundra_rd_u32(TundraReader *r) {
    if (!r->ok || r->len - r->pos < 4) { r->ok = 0; return 0; }
    const uint8_t *p = r->p + r->pos;
    r->pos += 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
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

/* ---- bounds-checked payload writer ----
 * Mirrors the reader: once a write would overflow, `ok` stays 0. */
typedef struct {
    uint8_t *p;
    size_t cap;
    size_t len;
    int ok;
} TundraWriter;

static inline TundraWriter tundra_writer(uint8_t *p, size_t cap) {
    TundraWriter w = { p, cap, 0, 1 };
    return w;
}

static inline uint8_t *tundra_wr_reserve(TundraWriter *w, size_t n) {
    if (!w->ok || w->cap - w->len < n) { w->ok = 0; return NULL; }
    uint8_t *b = w->p + w->len;
    w->len += n;
    return b;
}

static inline void tundra_wr_u8(TundraWriter *w, uint8_t v) {
    uint8_t *b = tundra_wr_reserve(w, 1);
    if (b) b[0] = v;
}

static inline void tundra_wr_u16(TundraWriter *w, uint16_t v) {
    uint8_t *b = tundra_wr_reserve(w, 2);
    if (b) tundra_put_u16(b, v);
}

static inline void tundra_wr_u32(TundraWriter *w, uint32_t v) {
    uint8_t *b = tundra_wr_reserve(w, 4);
    if (b) tundra_put_u32(b, v);
}

static inline void tundra_wr_u64(TundraWriter *w, uint64_t v) {
    uint8_t *b = tundra_wr_reserve(w, 8);
    if (b) tundra_put_u64(b, v);
}

static inline void tundra_wr_bytes(TundraWriter *w, const void *src, size_t n) {
    uint8_t *b = tundra_wr_reserve(w, n);
    if (b && n) memcpy(b, src, n);
}

#endif /* TUNDRA_PROTOCOL_H */
