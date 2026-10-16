/*
 * In-memory log of the most recent chat messages. Message ids are assigned
 * here and increase by one per message, so an id maps directly to a slot.
 */
#include <stdlib.h>
#include <string.h>

#include "server.h"

static LoggedMessage ring[MSGLOG_CAPACITY];
static uint64_t next_id = 1;   /* id of the next message */
static uint64_t first_id = 1;  /* oldest id still held */
static size_t total_bytes = 0; /* text bytes held */

static LoggedMessage *slot(uint64_t id) {
    return &ring[(id - 1) % MSGLOG_CAPACITY];
}

static void evict_oldest(void) {
    LoggedMessage *m = slot(first_id);
    if (m->id == first_id) {
        total_bytes -= m->text_len;
        free(m->text);
        memset(m, 0, sizeof(*m));
    }
    first_id++;
}

static void copy_name(char *dst, size_t cap, TundraBytes src) {
    size_t n = src.len < cap - 1 ? src.len : cap - 1;
    memcpy(dst, src.p, n);
    dst[n] = '\0';
}

const LoggedMessage *msglog_append(uint8_t kind, TundraBytes target, const char *sender, TundraBytes text) {
    uint8_t *copy = malloc(text.len ? text.len : 1);
    if (!copy) return NULL;
    if (text.len) memcpy(copy, text.p, text.len);

    /* make room: by count (the slot we are about to reuse) and by bytes */
    while (first_id < next_id &&
           (next_id - first_id >= MSGLOG_CAPACITY || total_bytes + text.len > MSGLOG_MAX_BYTES)) {
        evict_oldest();
    }

    uint64_t id = next_id++;
    LoggedMessage *m = slot(id);
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->ts_ms = wall_ms();
    m->kind = kind;
    copy_name(m->target, sizeof(m->target), target);
    strncpy(m->sender, sender, sizeof(m->sender) - 1);
    m->text = copy;
    m->text_len = text.len;
    total_bytes += text.len;
    return m;
}

const LoggedMessage *msglog_get(uint64_t id) {
    if (id < first_id || id >= next_id) return NULL;
    LoggedMessage *m = slot(id);
    return m->id == id ? m : NULL;
}

uint64_t msglog_last_id(void) {
    return next_id - 1;
}

uint64_t msglog_first_id(void) {
    return first_id;
}

void msglog_to_chat(const LoggedMessage *m, TundraMsg *out) {
    memset(out, 0, sizeof(*out));
    out->type = MSG_CHAT;
    out->u.chat.id = m->id;
    out->u.chat.ts_ms = m->ts_ms;
    out->u.chat.kind = m->kind;
    out->u.chat.target.p = (const uint8_t *)m->target;
    out->u.chat.target.len = strlen(m->target);
    out->u.chat.sender.p = (const uint8_t *)m->sender;
    out->u.chat.sender.len = strlen(m->sender);
    out->u.chat.text.p = m->text;
    out->u.chat.text.len = m->text_len;
}

void msglog_free(void) {
    for (int i = 0; i < MSGLOG_CAPACITY; i++) {
        free(ring[i].text);
        ring[i].text = NULL;
        ring[i].id = 0;
    }
    total_bytes = 0;
}
