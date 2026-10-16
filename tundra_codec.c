#include "tundra_codec.h"

#include <string.h>

/* ---- field helpers ---- */

static TundraBytes rd_str8(TundraReader *r) {
    TundraBytes b;
    b.len = tundra_rd_u8(r);
    b.p = tundra_rd_bytes(r, b.len);
    return b;
}

static TundraBytes rd_str16(TundraReader *r) {
    TundraBytes b;
    b.len = tundra_rd_u16(r);
    b.p = tundra_rd_bytes(r, b.len);
    return b;
}

static void wr_str8(TundraWriter *w, TundraBytes b) {
    if (b.len > 0xFF) { w->ok = 0; return; }
    tundra_wr_u8(w, (uint8_t)b.len);
    tundra_wr_bytes(w, b.p, b.len);
}

static void wr_str16(TundraWriter *w, TundraBytes b) {
    if (b.len > 0xFFFF) { w->ok = 0; return; }
    tundra_wr_u16(w, (uint16_t)b.len);
    tundra_wr_bytes(w, b.p, b.len);
}

/* ---- list entries ---- */

static int read_chat(TundraReader *r, TundraMsg *out) {
    memset(out, 0, sizeof(*out));
    out->type = MSG_CHAT;
    out->u.chat.id = tundra_rd_u64(r);
    out->u.chat.ts_ms = tundra_rd_u64(r);
    out->u.chat.kind = tundra_rd_u8(r);
    out->u.chat.target = rd_str8(r);
    out->u.chat.sender = rd_str8(r);
    out->u.chat.text = rd_str16(r);
    return r->ok;
}

int tundra_next_room(TundraReader *it, TundraBytes *room, uint16_t *members) {
    if (it->pos >= it->len) return 0;
    *room = rd_str8(it);
    *members = tundra_rd_u16(it);
    return it->ok;
}

int tundra_next_who(TundraReader *it, TundraBytes *user, uint8_t *online) {
    if (it->pos >= it->len) return 0;
    *user = rd_str8(it);
    *online = tundra_rd_u8(it);
    return it->ok;
}

int tundra_next_agent(TundraReader *it, TundraBytes *name, uint8_t *online, TundraBytes *capabilities) {
    if (it->pos >= it->len) return 0;
    *name = rd_str8(it);
    *online = tundra_rd_u8(it);
    *capabilities = rd_str16(it);
    return it->ok;
}

int tundra_next_history(TundraReader *it, TundraMsg *chat) {
    if (it->pos >= it->len) return 0;
    uint16_t n = tundra_rd_u16(it);
    const uint8_t *p = tundra_rd_bytes(it, n);
    if (!p) return 0;
    TundraReader sub = tundra_reader(p, n);
    return read_chat(&sub, chat) && tundra_rd_done(&sub);
}

/* ---- REGISTER extensions ---- */

static int tlv_valid(TundraBytes ext) {
    TundraReader r = tundra_reader(ext.p, ext.len);
    while (r.ok && r.pos < r.len) {
        (void)tundra_rd_u8(&r);
        uint16_t n = tundra_rd_u16(&r);
        (void)tundra_rd_bytes(&r, n);
    }
    return r.ok;
}

int tundra_reg_find(TundraBytes ext, uint8_t tag, TundraBytes *value) {
    TundraReader r = tundra_reader(ext.p, ext.len);
    while (r.ok && r.pos < r.len) {
        uint8_t t = tundra_rd_u8(&r);
        uint16_t n = tundra_rd_u16(&r);
        const uint8_t *v = tundra_rd_bytes(&r, n);
        if (r.ok && t == tag) {
            value->p = v;
            value->len = n;
            return 1;
        }
    }
    return 0;
}

void tundra_wr_tlv(TundraWriter *w, uint8_t tag, const void *value, size_t len) {
    if (len > 0xFFFF) { w->ok = 0; return; }
    tundra_wr_u8(w, tag);
    tundra_wr_u16(w, (uint16_t)len);
    tundra_wr_bytes(w, value, len);
}

/* Reads `count` list entries, validating each, and records their span. */
static void read_list(TundraReader *r, uint8_t type, uint16_t count, TundraBytes *entries) {
    size_t start = r->pos;
    for (uint16_t i = 0; i < count && r->ok; i++) {
        switch (type) {
            case MSG_ROOM_LIST:
                (void)rd_str8(r);
                (void)tundra_rd_u16(r);
                break;
            case MSG_WHO_RESULT:
                (void)rd_str8(r);
                (void)tundra_rd_u8(r);
                break;
            case MSG_AGENT_LIST:
                (void)rd_str8(r);
                (void)tundra_rd_u8(r);
                (void)rd_str16(r);
                break;
            case MSG_HISTORY_RESULT: {
                uint16_t n = tundra_rd_u16(r);
                const uint8_t *p = tundra_rd_bytes(r, n);
                if (!p) break;
                TundraReader sub = tundra_reader(p, n);
                TundraMsg chat;
                if (!read_chat(&sub, &chat) || !tundra_rd_done(&sub)) r->ok = 0;
                break;
            }
        }
    }
    entries->p = r->p + start;
    entries->len = r->ok ? r->pos - start : 0;
}

/* ---- decode ---- */

int tundra_decode(uint8_t type, const uint8_t *payload, size_t len, TundraMsg *out) {
    TundraReader r = tundra_reader(payload, len);
    memset(out, 0, sizeof(*out));
    out->type = type;

    switch (type) {
        case MSG_JOIN:
        case MSG_LEAVE:
        case MSG_WHO:
            out->u.room.room = rd_str8(&r);
            break;
        case MSG_SEND:
            out->u.send.kind = tundra_rd_u8(&r);
            out->u.send.target = rd_str8(&r);
            out->u.send.text = rd_str16(&r);
            break;
        case MSG_LIST_ROOMS:
            break;
        case MSG_HISTORY:
            out->u.history.room = rd_str8(&r);
            out->u.history.before_id = tundra_rd_u64(&r);
            out->u.history.limit = tundra_rd_u16(&r);
            break;
        case MSG_CHAT:
            (void)read_chat(&r, out);
            break;
        case MSG_SENT:
        case MSG_ACK:
            out->u.sent.id = tundra_rd_u64(&r);
            break;
        case MSG_ROOM_EVENT:
            out->u.room_event.event = tundra_rd_u8(&r);
            out->u.room_event.room = rd_str8(&r);
            out->u.room_event.user = rd_str8(&r);
            break;
        case MSG_ROOM_LIST:
        case MSG_AGENT_LIST:
            out->u.list.count = tundra_rd_u16(&r);
            read_list(&r, type, out->u.list.count, &out->u.list.entries);
            break;
        case MSG_AGENT_ADVERTISE:
            out->u.advertise.capabilities = rd_str16(&r);
            break;
        case MSG_AGENT_REQUEST:
        case MSG_AGENT_REQUEST_IN:
            out->u.agent_req.peer = rd_str8(&r);
            out->u.agent_req.req_id = tundra_rd_u64(&r);
            out->u.agent_req.body = rd_str16(&r);
            break;
        case MSG_AGENT_RESPONSE:
        case MSG_AGENT_RESPONSE_IN:
            out->u.agent_resp.peer = rd_str8(&r);
            out->u.agent_resp.req_id = tundra_rd_u64(&r);
            out->u.agent_resp.status = tundra_rd_u8(&r);
            out->u.agent_resp.final = tundra_rd_u8(&r);
            out->u.agent_resp.body = rd_str16(&r);
            break;
        case MSG_LIST_AGENTS:
            break;
        case MSG_WHO_RESULT:
        case MSG_HISTORY_RESULT:
            out->u.list.room = rd_str8(&r);
            out->u.list.count = tundra_rd_u16(&r);
            read_list(&r, type, out->u.list.count, &out->u.list.entries);
            break;
        case MSG_REGISTER: {
            const uint8_t *id = tundra_rd_bytes(&r, TUNDRA_UUID_LEN);
            if (id) memcpy(out->u.reg.id, id, TUNDRA_UUID_LEN);
            out->u.reg.username = rd_str8(&r);
            if (r.ok) {
                out->u.reg.ext.p = r.p + r.pos;
                out->u.reg.ext.len = r.len - r.pos;
                if (!tlv_valid(out->u.reg.ext)) r.ok = 0;
                r.pos = r.len;
            }
            break;
        }
        case MSG_MESSAGE:
            out->u.message.text = rd_str16(&r);
            break;
        case MSG_AGENT_COMMAND:
            out->u.agent_cmd.agent = rd_str8(&r);
            out->u.agent_cmd.command = rd_str16(&r);
            break;
        case MSG_PONG:
            out->u.pong.data.p = payload;
            out->u.pong.data.len = len;
            r.pos = len;
            break;
        case MSG_HEALTH_CHECK:
            break;
        case MSG_SYSTEM: {
            const uint8_t *id = tundra_rd_bytes(&r, TUNDRA_UUID_LEN);
            if (id) memcpy(out->u.system.id, id, TUNDRA_UUID_LEN);
            out->u.system.text = rd_str16(&r);
            break;
        }
        case MSG_BROADCAST_MESSAGE:
        case MSG_BROADCAST_AGENT:
            out->u.broadcast.sender = rd_str8(&r);
            out->u.broadcast.ts_ms = tundra_rd_u64(&r);
            out->u.broadcast.text = rd_str16(&r);
            break;
        case MSG_PING:
            out->u.ping.ts_ms = tundra_rd_u64(&r);
            break;
        case MSG_ERROR:
            out->u.error.code = tundra_rd_u16(&r);
            out->u.error.text = rd_str16(&r);
            break;
        case MSG_HEALTH_RESULT:
            out->u.health.ts_ms = tundra_rd_u64(&r);
            out->u.health.status = tundra_rd_u8(&r);
            out->u.health.latency_ms = tundra_rd_u32(&r);
            out->u.health.uptime_ms = tundra_rd_u32(&r);
            out->u.health.sent = tundra_rd_u32(&r);
            out->u.health.received = tundra_rd_u32(&r);
            break;
        default:
            return TUNDRA_DECODE_UNKNOWN;
    }
    return tundra_rd_done(&r) ? TUNDRA_DECODE_OK : TUNDRA_DECODE_MALFORMED;
}

/* ---- encode ---- */

static void write_chat(TundraWriter *w, const TundraMsg *m) {
    tundra_wr_u64(w, m->u.chat.id);
    tundra_wr_u64(w, m->u.chat.ts_ms);
    tundra_wr_u8(w, m->u.chat.kind);
    wr_str8(w, m->u.chat.target);
    wr_str8(w, m->u.chat.sender);
    wr_str16(w, m->u.chat.text);
}

void tundra_wr_room_entry(TundraWriter *w, TundraBytes room, uint16_t members) {
    wr_str8(w, room);
    tundra_wr_u16(w, members);
}

void tundra_wr_agent_entry(TundraWriter *w, TundraBytes name, uint8_t online, TundraBytes capabilities) {
    wr_str8(w, name);
    tundra_wr_u8(w, online);
    wr_str16(w, capabilities);
}

void tundra_wr_who_entry(TundraWriter *w, TundraBytes user, uint8_t online) {
    wr_str8(w, user);
    tundra_wr_u8(w, online);
}

void tundra_wr_history_entry(TundraWriter *w, const TundraMsg *chat) {
    uint8_t *lenp = tundra_wr_reserve(w, 2);
    size_t start = w->len;
    write_chat(w, chat);
    if (lenp && w->ok) {
        size_t n = w->len - start;
        if (n > 0xFFFF) w->ok = 0;
        else tundra_put_u16(lenp, (uint16_t)n);
    }
}

long tundra_encode(const TundraMsg *m, uint8_t *buf, size_t cap) {
    TundraWriter w = tundra_writer(buf, cap);

    switch (m->type) {
        case MSG_JOIN:
        case MSG_LEAVE:
        case MSG_WHO:
            wr_str8(&w, m->u.room.room);
            break;
        case MSG_SEND:
            tundra_wr_u8(&w, m->u.send.kind);
            wr_str8(&w, m->u.send.target);
            wr_str16(&w, m->u.send.text);
            break;
        case MSG_LIST_ROOMS:
            break;
        case MSG_HISTORY:
            wr_str8(&w, m->u.history.room);
            tundra_wr_u64(&w, m->u.history.before_id);
            tundra_wr_u16(&w, m->u.history.limit);
            break;
        case MSG_CHAT:
            write_chat(&w, m);
            break;
        case MSG_SENT:
        case MSG_ACK:
            tundra_wr_u64(&w, m->u.sent.id);
            break;
        case MSG_ROOM_EVENT:
            tundra_wr_u8(&w, m->u.room_event.event);
            wr_str8(&w, m->u.room_event.room);
            wr_str8(&w, m->u.room_event.user);
            break;
        case MSG_ROOM_LIST:
        case MSG_AGENT_LIST:
            tundra_wr_u16(&w, m->u.list.count);
            tundra_wr_bytes(&w, m->u.list.entries.p, m->u.list.entries.len);
            break;
        case MSG_AGENT_ADVERTISE:
            wr_str16(&w, m->u.advertise.capabilities);
            break;
        case MSG_AGENT_REQUEST:
        case MSG_AGENT_REQUEST_IN:
            wr_str8(&w, m->u.agent_req.peer);
            tundra_wr_u64(&w, m->u.agent_req.req_id);
            wr_str16(&w, m->u.agent_req.body);
            break;
        case MSG_AGENT_RESPONSE:
        case MSG_AGENT_RESPONSE_IN:
            wr_str8(&w, m->u.agent_resp.peer);
            tundra_wr_u64(&w, m->u.agent_resp.req_id);
            tundra_wr_u8(&w, m->u.agent_resp.status);
            tundra_wr_u8(&w, m->u.agent_resp.final);
            wr_str16(&w, m->u.agent_resp.body);
            break;
        case MSG_LIST_AGENTS:
            break;
        case MSG_WHO_RESULT:
        case MSG_HISTORY_RESULT:
            wr_str8(&w, m->u.list.room);
            tundra_wr_u16(&w, m->u.list.count);
            tundra_wr_bytes(&w, m->u.list.entries.p, m->u.list.entries.len);
            break;
        case MSG_REGISTER:
            tundra_wr_bytes(&w, m->u.reg.id, TUNDRA_UUID_LEN);
            wr_str8(&w, m->u.reg.username);
            if (!tlv_valid(m->u.reg.ext)) w.ok = 0;
            tundra_wr_bytes(&w, m->u.reg.ext.p, m->u.reg.ext.len);
            break;
        case MSG_MESSAGE:
            wr_str16(&w, m->u.message.text);
            break;
        case MSG_AGENT_COMMAND:
            wr_str8(&w, m->u.agent_cmd.agent);
            wr_str16(&w, m->u.agent_cmd.command);
            break;
        case MSG_PONG:
            tundra_wr_bytes(&w, m->u.pong.data.p, m->u.pong.data.len);
            break;
        case MSG_HEALTH_CHECK:
            break;
        case MSG_SYSTEM:
            tundra_wr_bytes(&w, m->u.system.id, TUNDRA_UUID_LEN);
            wr_str16(&w, m->u.system.text);
            break;
        case MSG_BROADCAST_MESSAGE:
        case MSG_BROADCAST_AGENT:
            wr_str8(&w, m->u.broadcast.sender);
            tundra_wr_u64(&w, m->u.broadcast.ts_ms);
            wr_str16(&w, m->u.broadcast.text);
            break;
        case MSG_PING:
            tundra_wr_u64(&w, m->u.ping.ts_ms);
            break;
        case MSG_ERROR:
            tundra_wr_u16(&w, m->u.error.code);
            wr_str16(&w, m->u.error.text);
            break;
        case MSG_HEALTH_RESULT:
            tundra_wr_u64(&w, m->u.health.ts_ms);
            tundra_wr_u8(&w, m->u.health.status);
            tundra_wr_u32(&w, m->u.health.latency_ms);
            tundra_wr_u32(&w, m->u.health.uptime_ms);
            tundra_wr_u32(&w, m->u.health.sent);
            tundra_wr_u32(&w, m->u.health.received);
            break;
        default:
            return -1;
    }
    if (!w.ok || w.len > TUNDRA_MAX_PAYLOAD) return -1;
    return (long)w.len;
}

const char *tundra_type_name(uint8_t type) {
    switch (type) {
        case MSG_REGISTER:          return "REGISTER";
        case MSG_MESSAGE:           return "MESSAGE";
        case MSG_AGENT_COMMAND:     return "AGENT_COMMAND";
        case MSG_PONG:              return "PONG";
        case MSG_HEALTH_CHECK:      return "HEALTH_CHECK";
        case MSG_SYSTEM:            return "SYSTEM";
        case MSG_BROADCAST_MESSAGE: return "BROADCAST_MESSAGE";
        case MSG_BROADCAST_AGENT:   return "BROADCAST_AGENT";
        case MSG_IMAGE:             return "IMAGE";
        case MSG_PING:              return "PING";
        case MSG_ERROR:             return "ERROR";
        case MSG_HEALTH_RESULT:     return "HEALTH_RESULT";
        case MSG_JOIN:              return "JOIN";
        case MSG_LEAVE:             return "LEAVE";
        case MSG_SEND:              return "SEND";
        case MSG_LIST_ROOMS:        return "LIST_ROOMS";
        case MSG_ACK:               return "ACK";
        case MSG_HISTORY:           return "HISTORY";
        case MSG_WHO:               return "WHO";
        case MSG_CHAT:              return "CHAT";
        case MSG_SENT:              return "SENT";
        case MSG_ROOM_LIST:         return "ROOM_LIST";
        case MSG_ROOM_EVENT:        return "ROOM_EVENT";
        case MSG_HISTORY_RESULT:    return "HISTORY_RESULT";
        case MSG_WHO_RESULT:        return "WHO_RESULT";
        case MSG_AGENT_ADVERTISE:   return "AGENT_ADVERTISE";
        case MSG_AGENT_REQUEST:     return "AGENT_REQUEST";
        case MSG_AGENT_RESPONSE:    return "AGENT_RESPONSE";
        case MSG_LIST_AGENTS:       return "LIST_AGENTS";
        case MSG_AGENT_LIST:        return "AGENT_LIST";
        case MSG_AGENT_REQUEST_IN:  return "AGENT_REQUEST_IN";
        case MSG_AGENT_RESPONSE_IN: return "AGENT_RESPONSE_IN";
        default:                    return "UNKNOWN";
    }
}
