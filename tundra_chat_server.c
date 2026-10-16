#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "tundra_protocol.h"

#define DEFAULT_PORT          4242
#define MAX_CLIENTS           1000
#define MAX_SESSIONS          MAX_CLIENTS
#define IDLE_TIMEOUT          120       /* seconds without any frame before a connection is dropped */
#define RESUME_GRACE          120       /* seconds a disconnected session is held for reconnect */
#define PING_INTERVAL         30        /* seconds */
#define MAX_BUFFERED_MESSAGES 50        /* per disconnected session; oldest dropped first */
#define MAX_OUTBOUND_BYTES    (1 << 20) /* per client; slower clients are disconnected */
#define MAX_EVENTS            64

typedef enum { SESSION_FREE = 0, SESSION_ACTIVE, SESSION_DETACHED } SessionState;

typedef struct {
    uint8_t type;
    uint8_t *payload;
    size_t len;
} BufferedFrame;

struct Client;

typedef struct Session {
    SessionState state;
    uuid_t id;
    char username[TUNDRA_MAX_USERNAME + 1];
    struct Client *client;      /* set only while ACTIVE */
    time_t connected_at;
    time_t detached_at;
    uint16_t out_seq;           /* sequence counter for server-initiated frames */
    BufferedFrame backlog[MAX_BUFFERED_MESSAGES];
    size_t backlog_head;        /* index of oldest entry */
    size_t backlog_count;
    uint32_t latency_ms;
    uint32_t messages_sent;
    uint32_t messages_received;
} Session;

typedef struct Client {
    int fd;                     /* -1 when the slot is free */
    int dead;                   /* closed at the end of the current loop iteration */
    int close_after_flush;
    int want_write;             /* EPOLLOUT currently registered */
    Session *session;
    time_t last_seen;
    uint64_t ping_sent_ms;
    uint8_t *out;
    size_t out_len;
    size_t out_cap;
    size_t in_len;
    uint8_t in[TUNDRA_MAX_FRAME]; /* keep last: reset code clears everything before it */
} Client;

static Client clients[MAX_CLIENTS];
static Session sessions[MAX_SESSIONS];
static int epoll_fd = -1;
static int server_fd = -1;
static int verbose = 0;
static volatile sig_atomic_t running = 1;

/* Marker stored in epoll data.ptr for the listening socket. */
static int listen_marker;

static const uuid_t nil_uuid = {0};

/* ------------------------------------------------------------------ */
/* utilities                                                           */

static void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void log_debug(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static time_t mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/* client output                                                       */

static void set_epollout(Client *c, int on) {
    if (c->want_write == on || c->fd < 0) return;
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | (on ? EPOLLOUT : 0);
    ev.data.ptr = c;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) == 0) c->want_write = on;
}

static void mark_dead(Client *c);

static void flush_client(Client *c) {
    while (c->out_len > 0 && !c->dead) {
        ssize_t n = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_debug("send failed on fd %d: %s", c->fd, strerror(errno));
            mark_dead(c);
            return;
        }
        memmove(c->out, c->out + n, c->out_len - (size_t)n);
        c->out_len -= (size_t)n;
    }
    if (c->dead) return;
    set_epollout(c, c->out_len > 0);
    if (c->out_len == 0 && c->close_after_flush) mark_dead(c);
}

/* Appends one frame to the client's output queue and tries to send it. */
static void send_frame(Client *c, uint8_t type, uint16_t seq, const uint8_t *payload, size_t len) {
    if (c->dead || c->fd < 0) return;
    if (len > TUNDRA_MAX_PAYLOAD) {
        log_info("refusing to send oversized payload (%zu bytes)", len);
        return;
    }
    size_t need = c->out_len + TUNDRA_HEADER_LEN + len;
    if (need > MAX_OUTBOUND_BYTES) {
        log_info("fd %d exceeded outbound limit, disconnecting", c->fd);
        mark_dead(c);
        return;
    }
    if (need > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < need) cap *= 2;
        uint8_t *p = realloc(c->out, cap);
        if (!p) {
            mark_dead(c);
            return;
        }
        c->out = p;
        c->out_cap = cap;
    }
    tundra_put_header(c->out + c->out_len, type, seq, len);
    if (len) memcpy(c->out + c->out_len + TUNDRA_HEADER_LEN, payload, len);
    c->out_len = need;
    if (c->session) c->session->messages_sent++;
    log_debug("queued type 0x%02x seq %u len %zu to fd %d", type, seq, len, c->fd);
    flush_client(c);
}

/* Server-initiated frame: uses the session's own sequence counter. */
static void push_frame(Client *c, uint8_t type, const uint8_t *payload, size_t len) {
    uint16_t seq = c->session ? c->session->out_seq++ : 0;
    send_frame(c, type, seq, payload, len);
}

static void send_error(Client *c, uint16_t seq, uint16_t code, const char *msg) {
    uint8_t buf[4 + 256];
    size_t mlen = strlen(msg);
    if (mlen > 256) mlen = 256;
    tundra_put_u16(buf, code);
    tundra_put_u16(buf + 2, (uint16_t)mlen);
    memcpy(buf + 4, msg, mlen);
    send_frame(c, MSG_ERROR, seq, buf, 4 + mlen);
}

static void send_system(Client *c, uint16_t seq, const uuid_t id, const char *msg) {
    uint8_t buf[TUNDRA_UUID_LEN + 2 + 256];
    size_t mlen = strlen(msg);
    if (mlen > 256) mlen = 256;
    memcpy(buf, id, TUNDRA_UUID_LEN);
    tundra_put_u16(buf + TUNDRA_UUID_LEN, (uint16_t)mlen);
    memcpy(buf + TUNDRA_UUID_LEN + 2, msg, mlen);
    send_frame(c, MSG_SYSTEM, seq, buf, TUNDRA_UUID_LEN + 2 + mlen);
}

/* ------------------------------------------------------------------ */
/* sessions                                                            */

static void backlog_clear(Session *s) {
    for (size_t i = 0; i < s->backlog_count; i++) {
        free(s->backlog[(s->backlog_head + i) % MAX_BUFFERED_MESSAGES].payload);
    }
    s->backlog_head = 0;
    s->backlog_count = 0;
}

static void backlog_push(Session *s, uint8_t type, const uint8_t *payload, size_t len) {
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return;
    memcpy(copy, payload, len);
    if (s->backlog_count == MAX_BUFFERED_MESSAGES) {
        /* full: drop the oldest */
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

static void backlog_replay(Session *s) {
    Client *c = s->client;
    for (size_t i = 0; i < s->backlog_count; i++) {
        BufferedFrame *f = &s->backlog[(s->backlog_head + i) % MAX_BUFFERED_MESSAGES];
        if (c && !c->dead) push_frame(c, f->type, f->payload, f->len);
    }
    backlog_clear(s);
}

static Session *find_session_by_id(const uuid_t id) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state != SESSION_FREE && uuid_compare(sessions[i].id, id) == 0) return &sessions[i];
    }
    return NULL;
}

static Session *find_session_by_name(const char *name) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state != SESSION_FREE && strcmp(sessions[i].username, name) == 0) return &sessions[i];
    }
    return NULL;
}

static Session *alloc_session(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].state == SESSION_FREE) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            return &sessions[i];
        }
    }
    return NULL;
}

static void attach(Session *s, Client *c) {
    s->state = SESSION_ACTIVE;
    s->client = c;
    c->session = s;
}

/* Detaches a client from its session; the session is kept for RESUME_GRACE. */
static void detach(Client *c) {
    Session *s = c->session;
    if (!s) return;
    s->state = SESSION_DETACHED;
    s->detached_at = mono_s();
    s->client = NULL;
    c->session = NULL;
}

/* Sends a notice (SYSTEM frame with nil id) to every active session except `skip`. */
static void notice_all(const char *msg, const Session *skip) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (s->state != SESSION_ACTIVE || s == skip || !s->client) continue;
        uint16_t seq = s->out_seq++;
        send_system(s->client, seq, nil_uuid, msg);
    }
}

/* Builds a broadcast payload once and delivers it to every session except
 * the sender. Disconnected sessions get it in their backlog. */
static void broadcast(uint8_t type, const Session *sender, const uint8_t *text, size_t text_len) {
    size_t ulen = strlen(sender->username);
    size_t len = 1 + ulen + 8 + 2 + text_len;
    if (len > TUNDRA_MAX_PAYLOAD) return; /* callers already enforce TUNDRA_MAX_TEXT */

    uint8_t *payload = malloc(len);
    if (!payload) return;
    payload[0] = (uint8_t)ulen;
    memcpy(payload + 1, sender->username, ulen);
    tundra_put_u64(payload + 1 + ulen, wall_ms());
    tundra_put_u16(payload + 9 + ulen, (uint16_t)text_len);
    memcpy(payload + 11 + ulen, text, text_len);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &sessions[i];
        if (s == sender) continue;
        if (s->state == SESSION_ACTIVE && s->client) {
            push_frame(s->client, type, payload, len);
        } else if (s->state == SESSION_DETACHED) {
            backlog_push(s, type, payload, len);
        }
    }
    free(payload);
}

/* ------------------------------------------------------------------ */
/* client lifecycle                                                    */

static void mark_dead(Client *c) {
    if (c->dead) return;
    c->dead = 1;
    detach(c);
}

static void reap_client(Client *c) {
    if (c->fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        log_info("client on fd %d closed", c->fd);
    }
    free(c->out);
    memset(c, 0, offsetof(Client, in));
    c->fd = -1;
    c->in_len = 0;
}

static void reap_dead_clients(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && clients[i].dead) reap_client(&clients[i]);
    }
}

static Client *alloc_client(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            Client *c = &clients[i];
            memset(c, 0, offsetof(Client, in));
            c->in_len = 0;
            c->fd = fd;
            c->last_seen = mono_s();
            return c;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* message handlers                                                    */

static int valid_username(const uint8_t *name, size_t len) {
    if (len == 0 || len > TUNDRA_MAX_USERNAME) return 0;
    for (size_t i = 0; i < len; i++) {
        if (name[i] < 0x21 || name[i] > 0x7e) return 0; /* printable ASCII, no spaces */
    }
    return 1;
}

static void handle_register(Client *c, uint16_t seq, TundraReader *r) {
    const uint8_t *id = tundra_rd_bytes(r, TUNDRA_UUID_LEN);
    uint8_t ulen = tundra_rd_u8(r);
    const uint8_t *uname = tundra_rd_bytes(r, ulen);
    if (!tundra_rd_done(r) || !valid_username(uname, ulen)) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Invalid register payload");
        return;
    }
    if (c->session) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Already registered");
        return;
    }

    char username[TUNDRA_MAX_USERNAME + 1];
    memcpy(username, uname, ulen);
    username[ulen] = '\0';

    /* Resume: a non-nil id that matches a live session for the same user. */
    if (uuid_compare(id, nil_uuid) != 0) {
        Session *s = find_session_by_id(id);
        if (s && strcmp(s->username, username) == 0) {
            if (s->state == SESSION_ACTIVE && s->client && s->client != c) {
                /* the old connection is still attached (e.g. half-open); replace it */
                Client *old = s->client;
                mark_dead(old);
            }
            attach(s, c);
            char msg[96];
            snprintf(msg, sizeof(msg), "Welcome back, %s!", username);
            send_system(c, seq, s->id, msg);
            backlog_replay(s);
            log_info("%s resumed session", username);
            return;
        }
        /* unknown or expired id: fall through to a fresh registration */
    }

    if (find_session_by_name(username)) {
        send_error(c, seq, ERR_USERNAME_TAKEN, "Username already taken");
        c->close_after_flush = 1;
        flush_client(c);
        return;
    }

    Session *s = alloc_session();
    if (!s) {
        send_error(c, seq, ERR_SERVER_ERROR, "Server full");
        c->close_after_flush = 1;
        flush_client(c);
        return;
    }
    uuid_generate(s->id);
    memcpy(s->username, username, (size_t)ulen + 1);
    s->connected_at = mono_s();
    attach(s, c);

    char msg[96];
    snprintf(msg, sizeof(msg), "Welcome, %s!", username);
    send_system(c, seq, s->id, msg);

    snprintf(msg, sizeof(msg), "%s has joined the chat.", username);
    notice_all(msg, s);
    log_info("%s registered", username);
}

static void handle_message(Client *c, uint16_t seq, TundraReader *r) {
    uint16_t mlen = tundra_rd_u16(r);
    const uint8_t *text = tundra_rd_bytes(r, mlen);
    if (!tundra_rd_done(r)) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Invalid message payload");
        return;
    }
    if (mlen > TUNDRA_MAX_TEXT) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Message too long");
        return;
    }
    broadcast(MSG_BROADCAST_MESSAGE, c->session, text, mlen);
}

static void handle_agent_command(Client *c, uint16_t seq, TundraReader *r) {
    uint8_t alen = tundra_rd_u8(r);
    const uint8_t *agent = tundra_rd_bytes(r, alen);
    uint16_t clen = tundra_rd_u16(r);
    const uint8_t *cmd = tundra_rd_bytes(r, clen);
    if (!tundra_rd_done(r) || alen == 0) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Invalid agent command payload");
        return;
    }
    size_t text_len = (size_t)alen + 1 + clen;
    if (text_len > TUNDRA_MAX_TEXT) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Agent command too long");
        return;
    }
    uint8_t *text = malloc(text_len);
    if (!text) {
        send_error(c, seq, ERR_SERVER_ERROR, "Out of memory");
        return;
    }
    memcpy(text, agent, alen);
    text[alen] = ' ';
    memcpy(text + alen + 1, cmd, clen);
    broadcast(MSG_BROADCAST_AGENT, c->session, text, text_len);
    free(text);
}

static void handle_pong(Client *c) {
    if (c->ping_sent_ms) {
        uint64_t rtt = mono_ms() - c->ping_sent_ms;
        c->session->latency_ms = rtt > UINT32_MAX ? UINT32_MAX : (uint32_t)rtt;
        c->ping_sent_ms = 0;
    }
}

static void handle_health_check(Client *c, uint16_t seq) {
    Session *s = c->session;
    uint8_t buf[25];
    uint64_t uptime = (uint64_t)(mono_s() - s->connected_at) * 1000u;
    tundra_put_u64(buf, wall_ms());
    buf[8] = 0x01; /* connected */
    tundra_put_u32(buf + 9, s->latency_ms);
    tundra_put_u32(buf + 13, uptime > UINT32_MAX ? UINT32_MAX : (uint32_t)uptime);
    tundra_put_u32(buf + 17, s->messages_sent);
    tundra_put_u32(buf + 21, s->messages_received);
    send_frame(c, MSG_HEALTH_RESULT, seq, buf, sizeof(buf));
}

static void process_frame(Client *c, const uint8_t *frame, size_t payload_len) {
    uint8_t version = frame[0];
    uint16_t seq = tundra_get_u16(frame + 2);
    uint8_t type = frame[6];
    TundraReader r = tundra_reader(frame + TUNDRA_HEADER_LEN, payload_len);

    if (version != TUNDRA_VERSION) {
        send_error(c, seq, ERR_INVALID_FORMAT, "Unsupported protocol version");
        c->close_after_flush = 1;
        flush_client(c);
        return;
    }

    c->last_seen = mono_s();
    log_debug("fd %d: type 0x%02x seq %u len %zu", c->fd, type, seq, payload_len);

    if (type == MSG_REGISTER) {
        handle_register(c, seq, &r);
        return;
    }
    if (!c->session) {
        send_error(c, seq, ERR_NOT_REGISTERED, "Not registered");
        return;
    }
    c->session->messages_received++;

    switch (type) {
        case MSG_MESSAGE:       handle_message(c, seq, &r); break;
        case MSG_AGENT_COMMAND: handle_agent_command(c, seq, &r); break;
        case MSG_PONG:          handle_pong(c); break;
        case MSG_HEALTH_CHECK:  handle_health_check(c, seq); break;
        default:
            send_error(c, seq, ERR_INVALID_FORMAT, "Unknown message type");
            break;
    }
}

/* Processes every complete frame currently in the input buffer. */
static void process_input(Client *c) {
    size_t off = 0;
    while (!c->dead && !c->close_after_flush && c->in_len - off >= TUNDRA_HEADER_LEN) {
        size_t plen = tundra_get_u16(c->in + off + 4);
        if (c->in_len - off < TUNDRA_HEADER_LEN + plen) break;
        process_frame(c, c->in + off, plen);
        off += TUNDRA_HEADER_LEN + plen;
    }
    if (off > 0) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

static void read_client(Client *c) {
    while (!c->dead) {
        /* The buffer holds one maximum-size frame, and complete frames are
         * consumed after every read, so there is always room here. */
        ssize_t n = recv(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len, 0);
        if (n > 0) {
            c->in_len += (size_t)n;
            process_input(c);
            continue;
        }
        if (n == 0) {
            mark_dead(c);
            return;
        }
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) mark_dead(c);
        return;
    }
}

static void accept_clients(void) {
    for (;;) {
        int fd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) log_info("accept failed: %s", strerror(errno));
            return;
        }
        Client *c = alloc_client(fd);
        if (!c) {
            log_info("no free client slots, rejecting fd %d", fd);
            close(fd);
            continue;
        }
        struct epoll_event ev = {0};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            log_info("epoll add failed: %s", strerror(errno));
            close(fd);
            c->fd = -1;
            continue;
        }
        log_info("client connected on fd %d", fd);
    }
}

/* ------------------------------------------------------------------ */
/* periodic work                                                       */

static void expire_idle(void) {
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
            char msg[96];
            snprintf(msg, sizeof(msg), "%s has left the chat.", s->username);
            backlog_clear(s);
            s->state = SESSION_FREE;
            notice_all(msg, NULL);
            log_info("%s", msg);
        }
    }
}

static void send_pings(void) {
    uint8_t buf[8];
    tundra_put_u64(buf, wall_ms());
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd < 0 || c->dead || !c->session) continue;
        c->ping_sent_ms = mono_ms();
        push_frame(c, MSG_PING, buf, sizeof(buf));
    }
}

/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-p port] [-v]\n", prog);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    int opt;
    while ((opt = getopt(argc, argv, "p:vh")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'v': verbose = 1; break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (port <= 0 || port > 65535) {
        usage(argv[0]);
        return 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, 128) < 0) {
        perror("listen");
        return 1;
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = &listen_marker;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl");
        return 1;
    }

    log_info("Tundra server listening on port %d", port);

    struct epoll_event events[MAX_EVENTS];
    time_t last_sweep = mono_s();
    time_t last_ping = mono_s();

    while (running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0 && errno != EINTR) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == &listen_marker) {
                accept_clients();
                continue;
            }
            Client *c = events[i].data.ptr;
            if (c->fd < 0 || c->dead) continue;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                read_client(c); /* pick up any final bytes / EOF */
                mark_dead(c);
                continue;
            }
            if (events[i].events & EPOLLIN) read_client(c);
            if (!c->dead && (events[i].events & EPOLLOUT)) flush_client(c);
        }

        time_t now = mono_s();
        if (now - last_sweep >= 1) {
            expire_idle();
            last_sweep = now;
        }
        if (now - last_ping >= PING_INTERVAL) {
            send_pings();
            last_ping = now;
        }

        /* Close sockets only after the whole event batch is handled, so a
         * reused fd can never be confused with a client closed in this batch. */
        reap_dead_clients();
    }

    log_info("shutting down");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) {
            mark_dead(&clients[i]);
            reap_client(&clients[i]);
        }
    }
    for (int i = 0; i < MAX_SESSIONS; i++) backlog_clear(&sessions[i]);
    close(epoll_fd);
    close(server_fd);
    return 0;
}
