/*
 * Small scriptable Tundra client, used for manual testing and by tests/integration.sh.
 *
 *   tundra_client -u alice [-H host] [-p port] [-r session-id] [actions...] [-w seconds]
 *
 * Actions run in the order given, after registering:
 *   -j #room      join a room
 *   -L #room      leave a room
 *   -t target     set the target for following -m: "#room" or a username (default #lobby)
 *   -m text       send text to the current target
 *   -R            list rooms
 *   -W #room      list members of a room
 *   -y #room      fetch recent history of a room
 *   -c            health check
 * Then everything received is printed for -w seconds (default 1). PINGs are
 * answered automatically.
 *
 * Output is one line per frame (or list entry), e.g.
 *   SESSION 1b4e28ba-2fa1-11d2-883f-0016d3cca427
 *   SYSTEM Welcome, alice!
 *   EVENT joined #lobby alice
 *   SENT 12
 *   CHAT 12 #lobby alice hello          (room message)
 *   CHAT 13 @bob alice psst             (direct message to bob)
 *   ROOM #lobby 3
 *   WHO #lobby alice 1
 *   HIST 12 #lobby alice hello
 *   ERROR 6 No such room
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "tundra_codec.h"

#define MAX_ACTIONS 128

typedef struct {
    char op;
    const char *arg;
    const char *target; /* for 'm' */
} Action;

static int sock = -1;
static uint16_t next_seq = 0;
static uint8_t inbuf[TUNDRA_MAX_FRAME];
static size_t inlen = 0;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static TundraBytes str(const char *s) {
    TundraBytes b = { (const uint8_t *)s, strlen(s) };
    return b;
}

static int send_all(const uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(sock, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_raw(uint8_t type, const uint8_t *payload, size_t len) {
    static uint8_t frame[TUNDRA_MAX_FRAME];
    if (len > TUNDRA_MAX_PAYLOAD) return -1;
    tundra_put_header(frame, type, next_seq++, len);
    if (len) memcpy(frame + TUNDRA_HEADER_LEN, payload, len);
    return send_all(frame, TUNDRA_HEADER_LEN + len);
}

static int send_msg(const TundraMsg *m) {
    static uint8_t buf[TUNDRA_MAX_PAYLOAD];
    long n = tundra_encode(m, buf, sizeof(buf));
    if (n < 0) return -1;
    return send_raw(m->type, buf, (size_t)n);
}

static void print_chat(const char *tag, const TundraMsg *m) {
    printf("%s %llu %s%.*s %.*s %.*s\n", tag, (unsigned long long)m->u.chat.id,
           m->u.chat.kind == CHAT_DIRECT ? "@" : "",
           (int)m->u.chat.target.len, (const char *)m->u.chat.target.p,
           (int)m->u.chat.sender.len, (const char *)m->u.chat.sender.p,
           (int)m->u.chat.text.len, (const char *)m->u.chat.text.p);
}

/* Prints one frame. Returns the frame type, or -1 on a malformed frame. */
static int handle_frame(const uint8_t *frame, size_t plen) {
    uint8_t type = frame[6];
    TundraMsg m;
    if (tundra_decode(type, frame + TUNDRA_HEADER_LEN, plen, &m) != TUNDRA_DECODE_OK) {
        printf("UNKNOWN 0x%02x len=%zu\n", type, plen);
        fflush(stdout);
        return type;
    }

    switch (type) {
        case MSG_SYSTEM:
            if (!uuid_is_null(m.u.system.id)) {
                char s[37];
                uuid_unparse_lower(m.u.system.id, s);
                printf("SESSION %s\n", s);
            }
            printf("SYSTEM %.*s\n", (int)m.u.system.text.len, (const char *)m.u.system.text.p);
            break;
        case MSG_CHAT:
            print_chat("CHAT", &m);
            break;
        case MSG_SENT:
            printf("SENT %llu\n", (unsigned long long)m.u.sent.id);
            break;
        case MSG_ROOM_EVENT:
            printf("EVENT %s %.*s %.*s\n", m.u.room_event.event == ROOM_JOINED ? "joined" : "left",
                   (int)m.u.room_event.room.len, (const char *)m.u.room_event.room.p,
                   (int)m.u.room_event.user.len, (const char *)m.u.room_event.user.p);
            break;
        case MSG_ROOM_LIST: {
            TundraReader it = tundra_reader(m.u.list.entries.p, m.u.list.entries.len);
            TundraBytes room;
            uint16_t members;
            printf("ROOMS %u\n", m.u.list.count);
            while (tundra_next_room(&it, &room, &members)) {
                printf("ROOM %.*s %u\n", (int)room.len, (const char *)room.p, members);
            }
            break;
        }
        case MSG_WHO_RESULT: {
            TundraReader it = tundra_reader(m.u.list.entries.p, m.u.list.entries.len);
            TundraBytes user;
            uint8_t online;
            while (tundra_next_who(&it, &user, &online)) {
                printf("WHO %.*s %.*s %u\n", (int)m.u.list.room.len, (const char *)m.u.list.room.p,
                       (int)user.len, (const char *)user.p, online);
            }
            break;
        }
        case MSG_HISTORY_RESULT: {
            TundraReader it = tundra_reader(m.u.list.entries.p, m.u.list.entries.len);
            TundraMsg chat;
            printf("HISTORY %.*s %u\n", (int)m.u.list.room.len, (const char *)m.u.list.room.p, m.u.list.count);
            while (tundra_next_history(&it, &chat)) print_chat("HIST", &chat);
            break;
        }
        case MSG_BROADCAST_AGENT:
            printf("AGENT %.*s %.*s\n", (int)m.u.broadcast.sender.len, (const char *)m.u.broadcast.sender.p,
                   (int)m.u.broadcast.text.len, (const char *)m.u.broadcast.text.p);
            break;
        case MSG_ERROR:
            printf("ERROR %u %.*s\n", m.u.error.code, (int)m.u.error.text.len, (const char *)m.u.error.text.p);
            break;
        case MSG_PING:
            printf("PING\n");
            send_raw(MSG_PONG, frame + TUNDRA_HEADER_LEN, plen);
            break;
        case MSG_HEALTH_RESULT:
            printf("HEALTH status=%u latency_ms=%u uptime_ms=%u sent=%u received=%u\n", m.u.health.status,
                   m.u.health.latency_ms, m.u.health.uptime_ms, m.u.health.sent, m.u.health.received);
            break;
        default:
            printf("UNKNOWN 0x%02x len=%zu\n", type, plen);
            break;
    }
    fflush(stdout);
    return type;
}

/* Reads and handles frames until `deadline`. If `until_type` is
 * non-negative, returns as soon as a frame of that type (or ERROR) arrives.
 * Returns the last frame type handled, 0 on timeout, -1 on error/close. */
static int pump(long deadline, int until_type) {
    for (;;) {
        while (inlen >= TUNDRA_HEADER_LEN) {
            size_t plen = tundra_get_u16(inbuf + 4);
            if (inlen < TUNDRA_HEADER_LEN + plen) break;
            if (inbuf[0] != TUNDRA_VERSION) {
                fprintf(stderr, "bad protocol version %u\n", inbuf[0]);
                return -1;
            }
            int t = handle_frame(inbuf, plen);
            memmove(inbuf, inbuf + TUNDRA_HEADER_LEN + plen, inlen - TUNDRA_HEADER_LEN - plen);
            inlen -= TUNDRA_HEADER_LEN + plen;
            if (until_type >= 0 && (t == until_type || t == MSG_ERROR)) return t;
        }

        long wait = deadline - now_ms();
        if (wait <= 0) return 0;
        struct pollfd pfd = { sock, POLLIN, 0 };
        int pr = poll(&pfd, 1, (int)wait);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) return 0;
        ssize_t n = recv(sock, inbuf + inlen, sizeof(inbuf) - inlen, 0);
        if (n == 0) {
            printf("CLOSED\n");
            fflush(stdout);
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        inlen += (size_t)n;
    }
}

static int connect_to(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res, *ai;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int run_action(const Action *a) {
    TundraMsg m;
    memset(&m, 0, sizeof(m));
    switch (a->op) {
        case 'j': m.type = MSG_JOIN; m.u.room.room = str(a->arg); break;
        case 'L': m.type = MSG_LEAVE; m.u.room.room = str(a->arg); break;
        case 'W': m.type = MSG_WHO; m.u.room.room = str(a->arg); break;
        case 'R': m.type = MSG_LIST_ROOMS; break;
        case 'c': m.type = MSG_HEALTH_CHECK; break;
        case 'y':
            m.type = MSG_HISTORY;
            m.u.history.room = str(a->arg);
            m.u.history.limit = 50;
            break;
        case 'm':
            m.type = MSG_SEND;
            m.u.send.kind = a->target[0] == '#' ? CHAT_ROOM : CHAT_DIRECT;
            m.u.send.target = str(a->target);
            m.u.send.text = str(a->arg);
            break;
        default:
            return 0;
    }
    return send_msg(&m);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s -u username [-H host] [-p port] [-r session-id]\n"
            "          [-j #room] [-L #room] [-t target] [-m text] [-R] [-W #room] [-y #room] [-c]\n"
            "          [-w seconds]\n",
            prog);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    const char *port = "4242";
    const char *user = NULL;
    const char *resume = NULL;
    const char *target = "#lobby";
    Action actions[MAX_ACTIONS];
    int nactions = 0;
    double wait_s = 1.0;

    int opt;
    while ((opt = getopt(argc, argv, "H:p:u:r:j:L:t:m:RW:y:cw:h")) != -1) {
        switch (opt) {
            case 'H': host = optarg; break;
            case 'p': port = optarg; break;
            case 'u': user = optarg; break;
            case 'r': resume = optarg; break;
            case 't': target = optarg; break;
            case 'w': wait_s = atof(optarg); break;
            case 'j': case 'L': case 'm': case 'R': case 'W': case 'y': case 'c':
                if (nactions < MAX_ACTIONS) {
                    actions[nactions].op = (char)opt;
                    actions[nactions].arg = optarg;
                    actions[nactions].target = target;
                    nactions++;
                }
                break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (!user) {
        usage(argv[0]);
        return 2;
    }

    TundraMsg reg;
    memset(&reg, 0, sizeof(reg));
    reg.type = MSG_REGISTER;
    reg.u.reg.username = str(user);
    if (resume && uuid_parse(resume, reg.u.reg.id) != 0) {
        fprintf(stderr, "invalid session id: %s\n", resume);
        return 2;
    }

    sock = connect_to(host, port);
    if (sock < 0) {
        fprintf(stderr, "could not connect to %s:%s\n", host, port);
        return 1;
    }

    if (send_msg(&reg) < 0) {
        fprintf(stderr, "register failed\n");
        return 1;
    }
    int t = pump(now_ms() + 5000, MSG_SYSTEM);
    if (t != MSG_SYSTEM) {
        if (t == 0) fprintf(stderr, "no reply to register\n");
        close(sock);
        return 1;
    }

    for (int i = 0; i < nactions; i++) {
        if (run_action(&actions[i]) < 0) {
            fprintf(stderr, "send failed\n");
            return 1;
        }
    }

    pump(now_ms() + (long)(wait_s * 1000), -1);
    close(sock);
    return 0;
}
