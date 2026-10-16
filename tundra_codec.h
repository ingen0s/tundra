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
        struct {
            uint8_t id[TUNDRA_UUID_LEN];
            TundraBytes username;
            TundraBytes ext; /* validated TLVs; read with tundra_reg_find */
        } reg;                                                                      /* REGISTER */
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
        struct { TundraBytes room; } room;                                          /* JOIN, LEAVE, WHO */
        struct { uint8_t kind; TundraBytes target; TundraBytes text; } send;        /* SEND */
        struct { TundraBytes room; uint64_t before_id; uint16_t limit; } history;   /* HISTORY */
        struct {
            uint64_t id;
            uint64_t ts_ms;
            uint8_t kind;
            TundraBytes target;
            TundraBytes sender;
            TundraBytes text;
        } chat;                                                                     /* CHAT */
        struct { uint64_t id; } sent;                                               /* SENT, ACK */
        struct { uint8_t event; TundraBytes room; TundraBytes user; } room_event;   /* ROOM_EVENT */
        /* List messages: `entries` holds `count` encoded entries, read them
         * with the matching tundra_next_* iterator below. Decoding has
         * already validated every entry. ROOM_LIST leaves `room` empty. */
        /* AGENT_LIST also carries `truncated`: 1 when more agents exist
         * than fit in one frame. */
        struct { TundraBytes room; uint8_t truncated; uint16_t count; TundraBytes entries; } list; /* ROOM_LIST, HISTORY_RESULT, WHO_RESULT, AGENT_LIST */
        struct { TundraBytes capabilities; } advertise;                             /* AGENT_ADVERTISE */
        /* AGENT_REQUEST: peer = agent. AGENT_REQUEST_IN: peer = requester. */
        struct { TundraBytes peer; uint64_t req_id; TundraBytes body; } agent_req;
        /* AGENT_RESPONSE: peer = requester. AGENT_RESPONSE_IN: peer = agent. */
        struct {
            TundraBytes peer;
            uint64_t req_id;
            uint8_t status;
            uint8_t final;
            TundraBytes body;
        } agent_resp;
    } u;
} TundraMsg;

/* Entry iterators for list messages. Start with
 *   TundraReader it = tundra_reader(msg.u.list.entries.p, msg.u.list.entries.len);
 * and call until they return 0. */
int tundra_next_room(TundraReader *it, TundraBytes *room, uint16_t *members);
int tundra_next_who(TundraReader *it, TundraBytes *user, uint8_t *online);
int tundra_next_history(TundraReader *it, TundraMsg *chat); /* fills a MSG_CHAT */
int tundra_next_agent(TundraReader *it, TundraBytes *name, uint8_t *online, TundraBytes *capabilities);

/* REGISTER extensions. tundra_reg_find returns 1 and sets `value` when
 * `tag` is present (first occurrence). tundra_wr_tlv appends one TLV. */
int tundra_reg_find(TundraBytes ext, uint8_t tag, TundraBytes *value);
void tundra_wr_tlv(TundraWriter *w, uint8_t tag, const void *value, size_t len);

/* Entry writers for building list messages. */
void tundra_wr_room_entry(TundraWriter *w, TundraBytes room, uint16_t members);
void tundra_wr_who_entry(TundraWriter *w, TundraBytes user, uint8_t online);
void tundra_wr_history_entry(TundraWriter *w, const TundraMsg *chat);
void tundra_wr_agent_entry(TundraWriter *w, TundraBytes name, uint8_t online, TundraBytes capabilities);

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
