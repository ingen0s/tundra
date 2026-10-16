#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

static const uuid_t nil_uuid = {0};

static TundraBytes cstr(const char *s) {
    TundraBytes b = { (const uint8_t *)s, strlen(s) };
    return b;
}

/* ---- REGISTER ---- */

static void handle_register(Client *c, uint16_t seq, const TundraMsg *m) {
    TundraBytes uname = m->u.reg.username;
    if (!valid_username(uname.p, uname.len)) {
        send_error(c, seq, ERR_INVALID_NAME, "Invalid username");
        return;
    }
    if (c->session) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Already registered");
        return;
    }

    char username[TUNDRA_MAX_USERNAME + 1];
    memcpy(username, uname.p, uname.len);
    username[uname.len] = '\0';

    /* Resume: a non-nil id must match a live session for the same user. */
    if (uuid_compare(m->u.reg.id, nil_uuid) != 0) {
        Session *s = find_session_by_id(m->u.reg.id);
        if (!s || strcmp(s->username, username) != 0) {
            send_error(c, seq, ERR_SESSION_EXPIRED, "Session expired or unknown; register with a zero id");
            return;
        }
        if (s->state == SESSION_ACTIVE && s->client && s->client != c) {
            /* the old connection is still attached (e.g. half-open); replace it */
            mark_dead(s->client);
        }
        TundraBytes v;
        int has_from = tundra_reg_find(m->u.reg.ext, REG_RESUME_FROM, &v);
        if (has_from && v.len != 8) {
            send_error(c, seq, ERR_INVALID_FORMAT, "RESUME_FROM must be 8 bytes");
            return;
        }
        /* hold the replay until the welcome and any notices are out */
        s->pumping = 1;
        attach(s, c);
        char msg[96];
        snprintf(msg, sizeof(msg), "Welcome back, %s!", username);
        send_system(c, seq, s->id, msg);
        if (s->backlog_dropped) {
            snprintf(msg, sizeof(msg), "%u buffered event(s) were dropped while you were away",
                     s->backlog_dropped);
            send_system(c, seq, nil_uuid, msg);
            s->backlog_dropped = 0;
        }

        /* Catch up from what the client says it has, else from its last
         * ACK, else from what was fully written to the old connection. */
        uint64_t from;
        if (has_from) from = tundra_get_u64(v.p);
        else if (s->has_acked) from = s->acked_id;
        else from = s->written_id;
        if (from > msglog_last_id()) from = msglog_last_id();
        if (from < s->start_id) from = s->start_id; /* nothing from before the session existed */

        if (chat_replay_start(s, from)) {
            send_system(c, seq, nil_uuid, "Some earlier messages are no longer available");
        }
        s->pumping = 0;
        session_pump(s);
        log_info("%s resumed session, catching up after message %llu", username, (unsigned long long)from);
        return;
    }

    TundraBytes token;
    int has_token = tundra_reg_find(m->u.reg.ext, REG_TOKEN, &token);
    if (!auth_check(username, has_token ? &token : NULL)) {
        log_info("rejected registration for %s: bad or missing token", username);
        send_error(c, seq, ERR_UNAUTHORIZED, "Invalid or missing token");
        close_after_flush(c);
        return;
    }

    if (find_session_by_name(username)) {
        send_error(c, seq, ERR_USERNAME_TAKEN, "Username already taken");
        close_after_flush(c);
        return;
    }

    Session *s = alloc_session();
    if (!s) {
        send_error(c, seq, ERR_SERVER_ERROR, "Server full");
        close_after_flush(c);
        return;
    }
    uuid_generate(s->id);
    s->start_id = s->written_id = msglog_last_id();
    memcpy(s->username, username, uname.len + 1);
    s->connected_at = mono_s();
    attach(s, c);

    char msg[96];
    snprintf(msg, sizeof(msg), "Welcome, %s!", username);
    send_system(c, seq, s->id, msg);

    Room *lobby = room_get_or_create(cstr(LOBBY));
    if (lobby) {
        room_add(lobby, s);
        room_event(lobby, ROOM_JOINED, s);
    }
    log_info("%s registered", username);
}

/* ---- chat ---- */

static void send_chat(Client *c, uint16_t seq, uint8_t kind, TundraBytes target, TundraBytes text) {
    Session *me = c->session;
    if (text.len > TUNDRA_MAX_TEXT) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Message too long");
        return;
    }

    Room *room = NULL;
    Session *peer = NULL;
    if (kind == CHAT_ROOM) {
        room = room_find(target);
        if (!room) {
            send_error(c, seq, ERR_NO_SUCH_TARGET, "No such room");
            return;
        }
        if (!room_is_member(room, me)) {
            send_error(c, seq, ERR_NOT_A_MEMBER, "Join the room first");
            return;
        }
    } else if (kind == CHAT_DIRECT) {
        peer = find_session_by_bytes(target);
        if (!peer) {
            send_error(c, seq, ERR_NO_SUCH_TARGET, "No such user");
            return;
        }
    } else {
        send_error(c, seq, ERR_INVALID_FORMAT, "Unknown chat kind");
        return;
    }

    const LoggedMessage *lm = msglog_append(kind, target, me->username, text);
    if (!lm) {
        send_error(c, seq, ERR_SERVER_ERROR, "Out of memory");
        return;
    }

    TundraMsg chat;
    msglog_to_chat(lm, &chat);
    static uint8_t buf[TUNDRA_MAX_PAYLOAD];
    long n = tundra_encode(&chat, buf, sizeof(buf));
    if (n < 0) {
        send_error(c, seq, ERR_SERVER_ERROR, "Encoding failed");
        return;
    }

    TundraMsg sent = { .type = MSG_SENT };
    sent.u.sent.id = lm->id;
    send_msg(c, &sent, seq, 0);

    if (room) room_deliver(room, me, MSG_CHAT, buf, (size_t)n);
    else if (peer != me) deliver(peer, MSG_CHAT, buf, (size_t)n);
}

/* ---- rooms ---- */

static void handle_join(Client *c, uint16_t seq, TundraBytes name) {
    if (!valid_room_name(name.p, name.len)) {
        send_error(c, seq, ERR_INVALID_NAME, "Invalid room name");
        return;
    }
    Room *r = room_get_or_create(name);
    if (!r) {
        send_error(c, seq, ERR_SERVER_ERROR, "Too many rooms");
        return;
    }
    if (room_is_member(r, c->session)) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Already in that room");
        return;
    }
    room_add(r, c->session);
    room_event(r, ROOM_JOINED, c->session);
}

static void handle_leave(Client *c, uint16_t seq, TundraBytes name) {
    Room *r = room_find(name);
    if (!r || !room_is_member(r, c->session)) {
        send_error(c, seq, ERR_NOT_A_MEMBER, "Not in that room");
        return;
    }
    Session *me = c->session;
    room_remove(r, me);
    if (r->used) {
        room_event(r, ROOM_LEFT, me);
    } else {
        /* the room is gone; still confirm to the leaver, in order */
        TundraMsg ev = { .type = MSG_ROOM_EVENT };
        ev.u.room_event.event = ROOM_LEFT;
        ev.u.room_event.room = name;
        ev.u.room_event.user = cstr(me->username);
        uint8_t buf[128];
        long n = tundra_encode(&ev, buf, sizeof(buf));
        if (n >= 0) deliver(me, MSG_ROOM_EVENT, buf, (size_t)n);
    }
}

static void handle_list_rooms(Client *c, uint16_t seq) {
    static uint8_t entries[TUNDRA_MAX_PAYLOAD];
    TundraWriter w = tundra_writer(entries, TUNDRA_MAX_PAYLOAD - 2);
    uint16_t count = 0;
    int cursor = 0;
    Room *r;
    while ((r = rooms_iter(&cursor)) != NULL) {
        size_t before = w.len;
        tundra_wr_room_entry(&w, cstr(r->name), (uint16_t)r->member_count);
        if (!w.ok) {
            w.len = before;
            break;
        }
        count++;
    }
    TundraMsg m = { .type = MSG_ROOM_LIST };
    m.u.list.count = count;
    m.u.list.entries.p = entries;
    m.u.list.entries.len = w.len;
    send_msg(c, &m, seq, 0);
}

static void handle_who(Client *c, uint16_t seq, TundraBytes name) {
    Room *r = room_find(name);
    if (!r) {
        send_error(c, seq, ERR_NO_SUCH_TARGET, "No such room");
        return;
    }
    static uint8_t entries[TUNDRA_MAX_PAYLOAD];
    TundraWriter w = tundra_writer(entries, TUNDRA_MAX_PAYLOAD - 64);
    uint16_t count = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (s->state == SESSION_FREE || !room_is_member(r, s)) continue;
        size_t before = w.len;
        tundra_wr_who_entry(&w, cstr(s->username), s->state == SESSION_ACTIVE);
        if (!w.ok) {
            w.ok = 1; /* full: stop at the last member that fit */
            w.len = before;
            break;
        }
        count++;
    }
    TundraMsg m = { .type = MSG_WHO_RESULT };
    m.u.list.room = cstr(r->name);
    m.u.list.count = count;
    m.u.list.entries.p = entries;
    m.u.list.entries.len = w.len;
    send_msg(c, &m, seq, 0);
}

static size_t chat_entry_size(const LoggedMessage *lm) {
    /* len(2) id(8) ts(8) kind(1) target(1+n) sender(1+n) mlen(2) text */
    return 2 + 8 + 8 + 1 + 1 + strlen(lm->target) + 1 + strlen(lm->sender) + 2 + lm->text_len;
}

static void handle_history(Client *c, uint16_t seq, const TundraMsg *req) {
    Room *r = room_find(req->u.history.room);
    if (!r) {
        send_error(c, seq, ERR_NO_SUCH_TARGET, "No such room");
        return;
    }
    if (!room_is_member(r, c->session)) {
        send_error(c, seq, ERR_NOT_A_MEMBER, "Join the room first");
        return;
    }
    size_t limit = req->u.history.limit;
    if (limit == 0 || limit > TUNDRA_MAX_HISTORY) limit = TUNDRA_MAX_HISTORY;

    /* Collect newest first, from the database when there is one (it goes
     * back further), else from the in-memory log, until we have `limit`
     * messages for this room or the reply would not fit. */
    const LoggedMessage *picked[TUNDRA_MAX_HISTORY];
    static LoggedMessage rows[TUNDRA_MAX_HISTORY];
    size_t nrows = 0;
    size_t npicked = 0;
    size_t budget = TUNDRA_MAX_PAYLOAD - (1 + TUNDRA_MAX_ROOM + 2);
    if (store_enabled()) {
        nrows = store_history(r->name, req->u.history.before_id, limit, rows);
        for (size_t i = 0; i < nrows; i++) {
            if (rows[i].id < r->first_msg_id) break; /* from an earlier room of this name */
            size_t sz = chat_entry_size(&rows[i]);
            if (sz > budget) break;
            budget -= sz;
            picked[npicked++] = &rows[i];
        }
    } else {
        uint64_t id = req->u.history.before_id ? req->u.history.before_id - 1 : msglog_last_id();
        if (id > msglog_last_id()) id = msglog_last_id();
        for (; id > 0 && npicked < limit; id--) {
            const LoggedMessage *lm = msglog_get(id);
            if (!lm) break; /* evicted: nothing older is held either */
            if (lm->id < r->first_msg_id) break; /* older messages belong to an earlier room of this name */
            if (lm->kind != CHAT_ROOM || strcmp(lm->target, r->name) != 0) continue;
            size_t sz = chat_entry_size(lm);
            if (sz > budget) break;
            budget -= sz;
            picked[npicked++] = lm;
        }
    }

    static uint8_t entries[TUNDRA_MAX_PAYLOAD];
    TundraWriter w = tundra_writer(entries, sizeof(entries));
    for (size_t i = npicked; i-- > 0;) {
        TundraMsg chat;
        msglog_to_chat(picked[i], &chat);
        tundra_wr_history_entry(&w, &chat);
    }
    TundraMsg m = { .type = MSG_HISTORY_RESULT };
    m.u.list.room = cstr(r->name);
    m.u.list.count = w.ok ? (uint16_t)npicked : 0;
    m.u.list.entries.p = entries;
    m.u.list.entries.len = w.ok ? w.len : 0;
    send_msg(c, &m, seq, 0);
    for (size_t i = 0; i < nrows; i++) free(rows[i].text);
}

/* ---- legacy agent command: broadcast to the lobby ---- */

static void handle_agent_command(Client *c, uint16_t seq, const TundraMsg *m) {
    TundraBytes agent = m->u.agent_cmd.agent, cmd = m->u.agent_cmd.command;
    size_t text_len = agent.len + 1 + cmd.len;
    if (!valid_username(agent.p, agent.len) || text_len > TUNDRA_MAX_TEXT) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Invalid agent command");
        return;
    }
    Room *lobby = room_find_cstr(LOBBY);
    if (!lobby || !room_is_member(lobby, c->session)) {
        send_error(c, seq, ERR_NOT_A_MEMBER, "Join #lobby first");
        return;
    }
    uint8_t *text = malloc(text_len);
    if (!text) return;
    memcpy(text, agent.p, agent.len);
    text[agent.len] = ' ';
    memcpy(text + agent.len + 1, cmd.p, cmd.len);

    TundraMsg out = { .type = MSG_BROADCAST_AGENT };
    out.u.broadcast.sender = cstr(c->session->username);
    out.u.broadcast.ts_ms = wall_ms();
    out.u.broadcast.text.p = text;
    out.u.broadcast.text.len = text_len;
    static uint8_t buf[TUNDRA_MAX_PAYLOAD];
    long n = tundra_encode(&out, buf, sizeof(buf));
    free(text);
    if (n >= 0) room_deliver(lobby, c->session, MSG_BROADCAST_AGENT, buf, (size_t)n);
}

/* ---- misc ---- */

static void handle_ack(Client *c, uint64_t id) {
    Session *s = c->session;
    if (id > msglog_last_id()) id = msglog_last_id(); /* cannot ack the future */
    if (!s->has_acked || id > s->acked_id) s->acked_id = id;
    s->has_acked = 1;
}

/* Latency is computed from the timestamp the client echoes back, so a
 * late answer to an old ping cannot produce a low reading. */
static void handle_pong(Client *c, TundraBytes data) {
    if (data.len != 8) return;
    uint64_t sent = tundra_get_u64(data.p);
    uint64_t now = wall_ms();
    if (sent == 0 || sent > now) return;
    uint64_t rtt = now - sent;
    c->session->latency_ms = rtt > UINT32_MAX ? UINT32_MAX : (uint32_t)rtt;
}

static void handle_health_check(Client *c, uint16_t seq) {
    Session *s = c->session;
    uint64_t uptime = (uint64_t)(mono_s() - c->accepted_at) * 1000u;
    TundraMsg m = { .type = MSG_HEALTH_RESULT };
    m.u.health.ts_ms = wall_ms();
    m.u.health.status = 1;
    m.u.health.latency_ms = s->latency_ms;
    m.u.health.uptime_ms = uptime > UINT32_MAX ? UINT32_MAX : (uint32_t)uptime;
    m.u.health.sent = s->messages_sent;
    m.u.health.received = s->messages_received;
    send_msg(c, &m, seq, 0);
}

/* ---- dispatch ---- */

void handle_frame(Client *c, const uint8_t *frame, size_t payload_len) {
    uint8_t version = frame[0];
    uint16_t seq = tundra_get_u16(frame + 2);
    uint8_t type = frame[6];

    if (version != TUNDRA_VERSION) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Unsupported protocol version");
        close_after_flush(c);
        return;
    }
    log_debug("<- fd %d %s seq %u len %zu", c->fd, tundra_type_name(type), seq, payload_len);

    TundraMsg m;
    int rc = tundra_decode(type, frame + TUNDRA_HEADER_LEN, payload_len, &m);
    if (rc == TUNDRA_DECODE_UNKNOWN || type >= 0x10) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Unknown message type");
        return;
    }
    if (rc != TUNDRA_DECODE_OK) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Malformed payload");
        return;
    }

    if (type == MSG_REGISTER) {
        handle_register(c, seq, &m);
        return;
    }
    if (!c->session) {
        /* does not refresh last_seen: unregistered clients must register in time */
        send_error(c, seq, ERR_NOT_REGISTERED, "Not registered");
        return;
    }
    c->last_seen = mono_s();
    c->session->messages_received++;

    switch (type) {
        case MSG_MESSAGE:       send_chat(c, seq, CHAT_ROOM, cstr(LOBBY), m.u.message.text); break;
        case MSG_SEND:          send_chat(c, seq, m.u.send.kind, m.u.send.target, m.u.send.text); break;
        case MSG_JOIN:          handle_join(c, seq, m.u.room.room); break;
        case MSG_LEAVE:         handle_leave(c, seq, m.u.room.room); break;
        case MSG_LIST_ROOMS:    handle_list_rooms(c, seq); break;
        case MSG_WHO:           handle_who(c, seq, m.u.room.room); break;
        case MSG_HISTORY:       handle_history(c, seq, &m); break;
        case MSG_AGENT_COMMAND: handle_agent_command(c, seq, &m); break;
        case MSG_ACK:           handle_ack(c, m.u.sent.id); break;
        case MSG_PONG:          handle_pong(c, m.u.pong.data); break;
        case MSG_HEALTH_CHECK:  handle_health_check(c, seq); break;
        default:                send_error(c, seq, ERR_INVALID_FORMAT, "Unknown message type"); break;
    }
}
