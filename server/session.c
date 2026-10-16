#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

Session sessions[MAX_SESSIONS];

int session_index(const Session *s) {
    return (int)(s - sessions);
}

Session *find_session_by_id(const uuid_t id) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state != SESSION_FREE && uuid_compare(sessions[i].id, id) == 0) return &sessions[i];
    }
    return NULL;
}

Session *find_session_by_name(const char *name) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state != SESSION_FREE && strcmp(sessions[i].username, name) == 0) return &sessions[i];
    }
    return NULL;
}

Session *find_session_by_bytes(TundraBytes name) {
    if (name.len == 0 || name.len > TUNDRA_MAX_USERNAME) return NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (s->state != SESSION_FREE && strlen(s->username) == name.len &&
            memcmp(s->username, name.p, name.len) == 0) {
            return s;
        }
    }
    return NULL;
}

void release_session(Session *s) {
    log_info("%s session released", s->username);
    rooms_remove_session(s); /* sends "left" events, which may queue one for s itself */
    backlog_clear(s);        /* so clear afterwards */
    s->state = SESSION_FREE;
}

/* A free slot, or else the longest-detached session is released to make
 * room, so abandoned sessions cannot lock new users out. */
Session *alloc_session(void) {
    Session *oldest = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (s->state == SESSION_FREE) {
            memset(s, 0, sizeof(*s));
            return s;
        }
        if (s->state == SESSION_DETACHED && (!oldest || s->detached_at_ms < oldest->detached_at_ms)) oldest = s;
    }
    if (!oldest) return NULL;
    release_session(oldest);
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

void attach(Session *s, Client *c) {
    s->state = SESSION_ACTIVE;
    s->client = c;
    c->session = s;
}

void detach(Client *c) {
    Session *s = c->session;
    if (!s) return;
    s->state = SESSION_DETACHED;
    s->detached_at_ms = mono_ms();
    s->client = NULL;
    c->session = NULL;
}

/* ---- backlog ---- */

static size_t total_backlog_bytes = 0;

static BufferedFrame *backlog_at(Session *s, size_t i) {
    return &s->backlog[(s->backlog_head + i) % MAX_BUFFERED_MESSAGES];
}

/* Removes the oldest entry and returns it; the caller owns the payload. */
static BufferedFrame backlog_pop(Session *s) {
    BufferedFrame *f = backlog_at(s, 0);
    BufferedFrame item = *f;
    f->payload = NULL;
    s->backlog_head = (s->backlog_head + 1) % MAX_BUFFERED_MESSAGES;
    s->backlog_count--;
    s->backlog_bytes -= item.len;
    total_backlog_bytes -= item.len;
    return item;
}

void backlog_clear(Session *s) {
    while (s->backlog_count > 0) free(backlog_pop(s).payload);
    s->backlog_head = 0;
    s->backlog_dropped = 0;
}

static void backlog_push(Session *s, uint8_t type, const uint8_t *payload, size_t len) {
    /* make room in this session: by count and by bytes, oldest first */
    while (s->backlog_count > 0 &&
           (s->backlog_count == MAX_BUFFERED_MESSAGES || s->backlog_bytes + len > MAX_BACKLOG_BYTES)) {
        free(backlog_pop(s).payload);
        s->backlog_dropped++;
    }
    /* the global budget protects the server from many detached sessions */
    if (len > MAX_BACKLOG_BYTES || total_backlog_bytes + len > MAX_TOTAL_BACKLOG) {
        s->backlog_dropped++;
        return;
    }
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) {
        s->backlog_dropped++;
        return;
    }
    if (len) memcpy(copy, payload, len);
    BufferedFrame *f = backlog_at(s, s->backlog_count);
    f->type = type;
    f->payload = copy;
    f->len = len;
    f->after_id = msglog_last_id();
    s->backlog_count++;
    s->backlog_bytes += len;
    total_backlog_bytes += len;
}

/* Would this session have received `m` while connected? Uses current room
 * membership, which cannot change while a session is detached. */
static int chat_visible_to(const LoggedMessage *m, const Session *s) {
    if (strcmp(m->sender, s->username) == 0) return 0; /* own messages are confirmed with SENT */
    if (m->id <= s->start_id) return 0; /* before this session existed */
    if (m->kind == CHAT_DIRECT) return strcmp(m->target, s->username) == 0;
    Room *r = room_find_cstr(m->target);
    return r && room_is_member(r, s) && m->id > room_joined_after(r, s);
}

static int has_room(const Client *c, size_t next_len) {
    return c->out_len == 0 || c->out_len + TUNDRA_HEADER_LEN + next_len <= REPLAY_HIGH_WATER;
}

int session_has_pending(const Session *s) {
    return s->backlog_count > 0 || s->replay_next != 0;
}

void session_pump(Session *s) {
    if (s->pumping) return; /* re-entered via push_frame -> flush_client */
    s->pumping = 1;
    static uint8_t buf[TUNDRA_MAX_PAYLOAD];

    while (s->state == SESSION_ACTIVE && s->client && !s->client->dead) {
        /* the log may have moved on while we were paused: say so, skip ahead */
        if (s->replay_next != 0 && s->replay_next < msglog_first_id()) {
            static const uuid_t nil = {0};
            s->replay_next = msglog_first_id();
            send_system(s->client, 0, nil, "Some earlier messages are no longer available");
            continue;
        }
        if (s->replay_next != 0 && s->replay_next > msglog_last_id()) s->replay_next = 0; /* caught up */

        /* a buffered event goes out once every chat before it has */
        if (s->backlog_count > 0 && (s->replay_next == 0 || backlog_at(s, 0)->after_id < s->replay_next)) {
            if (!has_room(s->client, backlog_at(s, 0)->len)) break;
            BufferedFrame item = backlog_pop(s);
            push_frame(s->client, item.type, item.payload, item.len);
            free(item.payload);
            continue;
        }
        if (s->replay_next == 0) break; /* nothing pending */

        uint64_t id = s->replay_next;
        const LoggedMessage *m = msglog_get(id);
        if (!m || !chat_visible_to(m, s)) {
            s->replay_next = id + 1;
            continue;
        }
        TundraMsg chat;
        msglog_to_chat(m, &chat);
        long n = tundra_encode(&chat, buf, sizeof(buf));
        if (n < 0) {
            s->replay_next = id + 1;
            continue;
        }
        if (!has_room(s->client, (size_t)n)) break;
        s->replay_next = id + 1;
        push_frame(s->client, MSG_CHAT, buf, (size_t)n);
    }

    s->pumping = 0;
    /* frames dropped while this session was online but behind */
    if (!session_has_pending(s) && s->backlog_dropped && s->state == SESSION_ACTIVE && s->client &&
        !s->client->dead) {
        static const uuid_t nil = {0};
        char msg[96];
        snprintf(msg, sizeof(msg), "%u message(s) were dropped because you fell behind", s->backlog_dropped);
        s->backlog_dropped = 0;
        send_system(s->client, 0, nil, msg);
    }
}

int chat_replay_start(Session *s, uint64_t after_id) {
    uint64_t first = msglog_first_id();
    int gap = after_id + 1 < first;
    uint64_t start = gap ? first : after_id + 1;
    s->replay_next = start <= msglog_last_id() ? start : 0;
    return gap;
}

void deliver(Session *s, uint8_t type, const uint8_t *payload, size_t len) {
    if (s->state == SESSION_FREE) return;
    int online = s->state == SESSION_ACTIVE && s->client;

    if (type == MSG_CHAT) {
        if (online && !session_has_pending(s)) {
            push_frame(s->client, type, payload, len);
        } else if (online && s->replay_next == 0 && len >= 8) {
            /* only buffered frames are pending: replay this chat after them */
            s->replay_next = tundra_get_u64(payload);
            session_pump(s);
        }
        /* detached, or a replay already in progress that will reach it */
        return;
    }

    if (online && !session_has_pending(s)) {
        push_frame(s->client, type, payload, len);
    } else {
        backlog_push(s, type, payload, len);
        if (online) session_pump(s);
    }
}

/* ---- periodic work ---- */

void sessions_expire(void) {
    time_t now = mono_s();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd < 0 || c->dead) continue;
        if (!c->session && now - c->accepted_at > REGISTER_TIMEOUT) {
            log_info("fd %d did not register in time", c->fd);
            mark_dead(c);
        } else if (c->session && now - c->last_seen > idle_timeout) {
            log_info("fd %d idle timeout", c->fd);
            mark_dead(c);
        }
    }
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (s->state == SESSION_DETACHED && mono_ms() - s->detached_at_ms > (uint64_t)resume_grace * 1000u) release_session(s);
    }
}

void sessions_recheck_auth(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (s->state == SESSION_FREE) continue;
        TundraBytes tok = { s->auth_token, s->auth_token_len };
        if (auth_check(s->username, s->auth_token_len ? &tok : NULL)) continue;
        log_info("%s no longer passes authentication; ending the session", s->username);
        if (s->state == SESSION_ACTIVE && s->client) {
            Client *c = s->client;
            send_error(c, 0, ERR_UNAUTHORIZED, "Your token is no longer valid");
            close_after_flush(c);
            if (!c->dead) mark_dead(c); /* detach now; queued bytes are flushed best-effort */
        }
        release_session(s);
    }
}

void sessions_ping(void) {
    uint8_t buf[8];
    tundra_put_u64(buf, wall_ms());
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd < 0 || c->dead || !c->session) continue;
        push_frame(c, MSG_PING, buf, sizeof(buf));
    }
}
