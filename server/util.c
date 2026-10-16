#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>

#include "server.h"

int verbose = 0;

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void log_debug(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint64_t clock_ms(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t wall_ms(void) { return clock_ms(CLOCK_REALTIME); }
uint64_t mono_ms(void) { return clock_ms(CLOCK_MONOTONIC); }

time_t mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* 1 to 32 printable ASCII characters, no spaces, not starting with '#'. */
int valid_username(const uint8_t *name, size_t len) {
    if (!name || len == 0 || len > TUNDRA_MAX_USERNAME || name[0] == '#') return 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] < 0x21 || name[i] > 0x7e) return 0;
    }
    return 1;
}

/* '#' followed by 1 to 31 of [a-z0-9_-]. */
int valid_room_name(const uint8_t *name, size_t len) {
    if (!name || len < 2 || len > TUNDRA_MAX_ROOM || name[0] != '#') return 0;
    for (size_t i = 1; i < len; i++) {
        uint8_t ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return 0;
    }
    return 1;
}
