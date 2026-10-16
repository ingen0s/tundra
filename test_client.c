/*
 * Small scriptable Tundra client, used for manual testing and by tests/integration.sh.
 *
 *   tundra_client -u alice [-H host] [-p port] [-r session-id] [-m text]... [-c] [-w seconds]
 *
 * Registers (or resumes with -r), sends each -m message in order, optionally
 * requests a health check (-c), then prints everything it receives for -w
 * seconds (default 1). PINGs are answered automatically.
 *
 * Output is one line per frame, e.g.
 *   SESSION 1b4e28ba-2fa1-11d2-883f-0016d3cca427
 *   SYSTEM Welcome, alice!
 *   MSG bob hello there
 *   ERROR 1 Username already taken
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
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

#include "tundra_protocol.h"

#define MAX_MESSAGES 64

static int sock = -1;
static uint16_t next_seq = 0;
static uint8_t inbuf[TUNDRA_MAX_FRAME];
static size_t inlen = 0;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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

static int send_frame(uint8_t type, const uint8_t *payload, size_t len) {
    static uint8_t frame[TUNDRA_MAX_FRAME];
    if (len > TUNDRA_MAX_PAYLOAD) return -1;
    tundra_put_header(frame, type, next_seq++, len);
    if (len) memcpy(frame + TUNDRA_HEADER_LEN, payload, len);
    return send_all(frame, TUNDRA_HEADER_LEN + len);
}

static int send_register(const char *user, const uuid_t id) {
    size_t ulen = strlen(user);
    if (ulen == 0 || ulen > TUNDRA_MAX_USERNAME) return -1;
    uint8_t buf[TUNDRA_UUID_LEN + 1 + TUNDRA_MAX_USERNAME];
    memcpy(buf, id, TUNDRA_UUID_LEN);
    buf[TUNDRA_UUID_LEN] = (uint8_t)ulen;
    memcpy(buf + TUNDRA_UUID_LEN + 1, user, ulen);
    return send_frame(MSG_REGISTER, buf, TUNDRA_UUID_LEN + 1 + ulen);
}

static int send_text(const char *text) {
    size_t len = strlen(text);
    if (len > TUNDRA_MAX_TEXT) return -1;
    uint8_t *buf = malloc(2 + len);
    if (!buf) return -1;
    tundra_put_u16(buf, (uint16_t)len);
    memcpy(buf + 2, text, len);
    int rc = send_frame(MSG_MESSAGE, buf, 2 + len);
    free(buf);
    return rc;
}

/* Prints one frame. Returns the frame type, or -1 on a malformed frame. */
static int handle_frame(const uint8_t *frame, size_t plen) {
    uint8_t type = frame[6];
    TundraReader r = tundra_reader(frame + TUNDRA_HEADER_LEN, plen);

    switch (type) {
        case MSG_SYSTEM: {
            const uint8_t *id = tundra_rd_bytes(&r, TUNDRA_UUID_LEN);
            uint16_t mlen = tundra_rd_u16(&r);
            const uint8_t *msg = tundra_rd_bytes(&r, mlen);
            if (!tundra_rd_done(&r)) return -1;
            if (!uuid_is_null(id)) {
                char s[37];
                uuid_unparse_lower(id, s);
                printf("SESSION %s\n", s);
            }
            printf("SYSTEM %.*s\n", mlen, (const char *)msg);
            break;
        }
        case MSG_BROADCAST_MESSAGE:
        case MSG_BROADCAST_AGENT: {
            uint8_t ulen = tundra_rd_u8(&r);
            const uint8_t *user = tundra_rd_bytes(&r, ulen);
            (void)tundra_rd_u64(&r); /* timestamp */
            uint16_t mlen = tundra_rd_u16(&r);
            const uint8_t *msg = tundra_rd_bytes(&r, mlen);
            if (!tundra_rd_done(&r)) return -1;
            printf("%s %.*s %.*s\n", type == MSG_BROADCAST_MESSAGE ? "MSG" : "AGENT",
                   ulen, (const char *)user, mlen, (const char *)msg);
            break;
        }
        case MSG_ERROR: {
            uint16_t code = tundra_rd_u16(&r);
            uint16_t mlen = tundra_rd_u16(&r);
            const uint8_t *msg = tundra_rd_bytes(&r, mlen);
            if (!tundra_rd_done(&r)) return -1;
            printf("ERROR %u %.*s\n", code, mlen, (const char *)msg);
            break;
        }
        case MSG_PING:
            printf("PING\n");
            send_frame(MSG_PONG, frame + TUNDRA_HEADER_LEN, plen);
            break;
        case MSG_HEALTH_RESULT: {
            if (plen != 25) return -1;
            const uint8_t *p = frame + TUNDRA_HEADER_LEN;
            uint32_t v[4];
            for (int i = 0; i < 4; i++) {
                const uint8_t *q = p + 9 + 4 * i;
                v[i] = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
            }
            printf("HEALTH status=%u latency_ms=%u uptime_ms=%u sent=%u received=%u\n",
                   p[8], v[0], v[1], v[2], v[3]);
            break;
        }
        default:
            printf("UNKNOWN 0x%02x len=%zu\n", type, plen);
            break;
    }
    fflush(stdout);
    return type;
}

/* Reads and handles frames until `deadline` (monotonic ms). If `until_type`
 * is non-negative, returns as soon as a frame of that type (or ERROR) arrives.
 * Returns the last frame type handled, 0 on timeout, -1 on error/close. */
static int pump(long deadline, int until_type) {
    for (;;) {
        /* drain complete frames already buffered */
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
            if (t < 0) {
                fprintf(stderr, "malformed frame\n");
                return -1;
            }
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

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s -u username [-H host] [-p port] [-r session-id] [-m text]... [-c] [-w seconds]\n",
            prog);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    const char *port = "4242";
    const char *user = NULL;
    const char *resume = NULL;
    const char *messages[MAX_MESSAGES];
    int nmessages = 0;
    int health = 0;
    double wait_s = 1.0;

    int opt;
    while ((opt = getopt(argc, argv, "H:p:u:r:m:cw:h")) != -1) {
        switch (opt) {
            case 'H': host = optarg; break;
            case 'p': port = optarg; break;
            case 'u': user = optarg; break;
            case 'r': resume = optarg; break;
            case 'm':
                if (nmessages < MAX_MESSAGES) messages[nmessages++] = optarg;
                break;
            case 'c': health = 1; break;
            case 'w': wait_s = atof(optarg); break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (!user) {
        usage(argv[0]);
        return 2;
    }

    uuid_t id;
    uuid_clear(id);
    if (resume && uuid_parse(resume, id) != 0) {
        fprintf(stderr, "invalid session id: %s\n", resume);
        return 2;
    }

    sock = connect_to(host, port);
    if (sock < 0) {
        fprintf(stderr, "could not connect to %s:%s\n", host, port);
        return 1;
    }

    if (send_register(user, id) < 0) {
        fprintf(stderr, "register failed\n");
        return 1;
    }
    int t = pump(now_ms() + 5000, MSG_SYSTEM);
    if (t != MSG_SYSTEM) {
        if (t == 0) fprintf(stderr, "no reply to register\n");
        close(sock);
        return 1;
    }

    for (int i = 0; i < nmessages; i++) {
        if (send_text(messages[i]) < 0) {
            fprintf(stderr, "send failed\n");
            return 1;
        }
    }
    if (health && send_frame(MSG_HEALTH_CHECK, NULL, 0) < 0) {
        fprintf(stderr, "send failed\n");
        return 1;
    }

    pump(now_ms() + (long)(wait_s * 1000), -1);
    close(sock);
    return 0;
}
