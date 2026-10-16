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

Session *alloc_session(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state == SESSION_FREE) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            return &sessions[i];
        }
    }
    return NULL;
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
    s->detached_at = mono_s();
    s->client = NULL;
    c->session = NULL;
}

/* ---- backlog ---- */

void backlog_clear(Session *s) {
    for (size_t i = 0; i < s->backlog_count; i++) {
        free(s->backlog[(s->backlog_head + i) % MAX_BUFFERED_MESSAGES].payload);
    }
    s->backlog_head = 0;
    s->backlog_count = 0;
}

static void backlog_push(Session *s, uint8_t type, const uint8_t *payload, size_t len) {
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return;
    if (len) memcpy(copy, payload, len);
    if (s->backlog_count == MAX_BUFFERED_MESSAGES) {
        free(s->backlog[s->backlog_head].payload);
        s->backlog_head = (s->backlog_head + 1) % MAX_BUFFERED_MESSAGES;
        s->backlog_count--;
    }
    size_t idx = (s->backlog_head + s->backlog_count) % MAX_BUFFERED_MESSAGES;
    s->backlog[idx].type = type;
    s->backlog[idx].payload = copy;
    s->backlog[idx].len = len;
    s->backlog_count++;
}

void backlog_replay(Session *s) {
    /* Take ownership of the entries first: a send can fail and detach the
     * session, and must not see (or append to) the list being replayed. */
    BufferedFrame items[MAX_BUFFERED_MESSAGES];
    size_t count = s->backlog_count;
    for (size_t i = 0; i < count; i++) items[i] = s->backlog[(s->backlog_head + i) % MAX_BUFFERED_MESSAGES];
    s->backlog_head = 0;
    s->backlog_count = 0;

    for (size_t i = 0; i < count; i++) {
        if (s->state == SESSION_ACTIVE && s->client) push_frame(s->client, items[i].type, items[i].payload, items[i].len);
        free(items[i].payload);
    }
}

void deliver(Session *s, uint8_t type, const uint8_t *payload, size_t len) {
    if (s->state == SESSION_ACTIVE && s->client) {
        push_frame(s->client, type, payload, len);
    } else if (s->state == SESSION_DETACHED) {
        backlog_push(s, type, payload, len);
    }
}

/* ---- periodic work ---- */

void sessions_expire(void) {
    time_t now = mono_s();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd >= 0 && !c->dead && now - c->last_seen > IDLE_TIMEOUT) {
            log_info("fd %d idle timeout", c->fd);
            mark_dead(c);
        }
    }
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (s->state == SESSION_DETACHED && now - s->detached_at > RESUME_GRACE) {
            log_info("%s session expired", s->username);
            backlog_clear(s);
            rooms_remove_session(s); /* sends "left" events */
            s->state = SESSION_FREE;
        }
    }
}

void sessions_ping(void) {
    uint8_t buf[8];
    tundra_put_u64(buf, wall_ms());
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd < 0 || c->dead || !c->session) continue;
        c->ping_sent_ms = mono_ms();
        push_frame(c, MSG_PING, buf, sizeof(buf));
    }
}
