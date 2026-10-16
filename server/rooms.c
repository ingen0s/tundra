#include <string.h>

#include "server.h"

static Room rooms[MAX_ROOMS];

static int name_eq(const Room *r, TundraBytes name) {
    return strlen(r->name) == name.len && memcmp(r->name, name.p, name.len) == 0;
}

Room *room_find(TundraBytes name) {
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (rooms[i].used && name_eq(&rooms[i], name)) return &rooms[i];
    }
    return NULL;
}

Room *room_find_cstr(const char *name) {
    TundraBytes b = { (const uint8_t *)name, strlen(name) };
    return room_find(b);
}

Room *room_get_or_create(TundraBytes name) {
    Room *r = room_find(name);
    if (r) return r;
    if (!valid_room_name(name.p, name.len)) return NULL;
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms[i].used) {
            r = &rooms[i];
            memset(r, 0, sizeof(*r));
            r->used = 1;
            memcpy(r->name, name.p, name.len);
            r->name[name.len] = '\0';
            return r;
        }
    }
    return NULL;
}

Room *rooms_iter(int *cursor) {
    while (*cursor < MAX_ROOMS) {
        Room *r = &rooms[(*cursor)++];
        if (r->used) return r;
    }
    return NULL;
}

int room_is_member(const Room *r, const Session *s) {
    int i = session_index(s);
    return (r->members[i / 8] >> (i % 8)) & 1;
}

void room_add(Room *r, Session *s) {
    if (room_is_member(r, s)) return;
    int i = session_index(s);
    r->members[i / 8] |= (uint8_t)(1u << (i % 8));
    r->member_count++;
}

void room_remove(Room *r, Session *s) {
    if (!room_is_member(r, s)) return;
    int i = session_index(s);
    r->members[i / 8] &= (uint8_t)~(1u << (i % 8));
    r->member_count--;
    if (r->member_count == 0 && strcmp(r->name, LOBBY) != 0) r->used = 0;
}

void room_deliver(Room *r, const Session *skip, uint8_t type, const uint8_t *payload, size_t len) {
    for (int i = 0; i < MAX_SESSIONS && r->used; i++) {
        Session *s = &sessions[i];
        if (s == skip || s->state == SESSION_FREE || !room_is_member(r, s)) continue;
        deliver(s, type, payload, len);
    }
}

void room_event(Room *r, uint8_t event, const Session *who) {
    TundraMsg m = { .type = MSG_ROOM_EVENT };
    m.u.room_event.event = event;
    m.u.room_event.room.p = (const uint8_t *)r->name;
    m.u.room_event.room.len = strlen(r->name);
    m.u.room_event.user.p = (const uint8_t *)who->username;
    m.u.room_event.user.len = strlen(who->username);
    uint8_t buf[128];
    long n = tundra_encode(&m, buf, sizeof(buf));
    if (n < 0) return;
    room_deliver(r, who, MSG_ROOM_EVENT, buf, (size_t)n);
    /* the subject always hears about their own join/leave, member or not */
    if (who->state == SESSION_ACTIVE && who->client) push_frame(who->client, MSG_ROOM_EVENT, buf, (size_t)n);
}

void rooms_remove_session(Session *s) {
    for (int i = 0; i < MAX_ROOMS; i++) {
        Room *r = &rooms[i];
        if (!r->used || !room_is_member(r, s)) continue;
        room_remove(r, s);
        if (r->used) room_event(r, ROOM_LEFT, s);
    }
}
