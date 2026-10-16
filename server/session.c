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
    backlog_clear(s);
    rooms_remove_session(s); /* sends "left" events */
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
    s->backlog_count++;
    s->backlog_bytes += len;
    total_backlog_bytes += len;
}

void backlog_pump(Session *s) {
    if (s->pumping) return; /* re-entered via push_frame -> flush_client */
    s->pumping = 1;
    while (s->backlog_count > 0 && s->state == SESSION_ACTIVE && s->client && !s->client->dead) {
        Client *c = s->client;
        if (c->out_len > 0 && c->out_len + TUNDRA_HEADER_LEN + backlog_at(s, 0)->len > REPLAY_HIGH_WATER) break;
        BufferedFrame item = backlog_pop(s);
        push_frame(c, item.type, item.payload, item.len);
        free(item.payload);
    }
    s->pumping = 0;
}

void deliver(Session *s, uint8_t type, const uint8_t *payload, size_t len) {
    if (s->state == SESSION_ACTIVE && s->client && s->backlog_count == 0) {
        push_frame(s->client, type, payload, len);
    } else if (s->state != SESSION_FREE) {
        backlog_push(s, type, payload, len);
        if (s->state == SESSION_ACTIVE) backlog_pump(s);
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

void sessions_ping(void) {
    uint8_t buf[8];
    tundra_put_u64(buf, wall_ms());
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd < 0 || c->dead || !c->session) continue;
        push_frame(c, MSG_PING, buf, sizeof(buf));
    }
}
