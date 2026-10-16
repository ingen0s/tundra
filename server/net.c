#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"

#define MAX_EVENTS 64

Client clients[MAX_CLIENTS];

static int epoll_fd = -1;
static int server_fd = -1;
static int ws_fd = -1;
static volatile sig_atomic_t running = 1;
static int listen_marker;    /* epoll data.ptr for the Tundra listener */
static int ws_listen_marker; /* epoll data.ptr for the HTTP/WebSocket listener */

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/* output                                                              */

static void set_epollout(Client *c, int on) {
    if (c->want_write == on || c->fd < 0) return;
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | (on ? EPOLLOUT : 0);
    ev.data.ptr = c;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) == 0) c->want_write = on;
}

/* Records that a CHAT frame ending at `end` in the output queue carries
 * `id`. When the table is full the oldest mark is dropped, which only makes
 * the written watermark more conservative. */
static void add_mark(Client *c, size_t end, uint64_t id) {
    if (c->marks_count == MAX_WRITE_MARKS) {
        c->marks_head = (c->marks_head + 1) % MAX_WRITE_MARKS;
        c->marks_count--;
    }
    size_t i = (c->marks_head + c->marks_count) % MAX_WRITE_MARKS;
    c->marks[i].end = end;
    c->marks[i].id = id;
    c->marks_count++;
}

/* `sent` bytes left the queue: every CHAT frame that ended within them is
 * now written, and the remaining marks shift down. */
static void advance_marks(Client *c, size_t sent) {
    while (c->marks_count > 0 && c->marks[c->marks_head].end <= sent) {
        uint64_t id = c->marks[c->marks_head].id;
        if (c->session && id > c->session->written_id) c->session->written_id = id;
        c->marks_head = (c->marks_head + 1) % MAX_WRITE_MARKS;
        c->marks_count--;
    }
    for (size_t i = 0; i < c->marks_count; i++) c->marks[(c->marks_head + i) % MAX_WRITE_MARKS].end -= sent;
}

static void flush_client(Client *c) {
    size_t off = 0;
    while (off < c->out_len && !c->dead) {
        ssize_t n = send(c->fd, c->out + off, c->out_len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_debug("send failed on fd %d: %s", c->fd, strerror(errno));
            mark_dead(c);
            return;
        }
        off += (size_t)n;
    }
    if (off > 0) {
        memmove(c->out, c->out + off, c->out_len - off);
        c->out_len -= off;
        advance_marks(c, off);
    }
    if (c->dead) return;
    /* room freed up: continue any backlog or chat replay */
    if (c->session && session_has_pending(c->session) && c->out_len < REPLAY_HIGH_WATER) {
        session_pump(c->session);
        if (c->dead) return;
    }
    set_epollout(c, c->out_len > 0);
    if (c->out_len == 0 && c->close_after_flush) mark_dead(c);
}

void close_after_flush(Client *c) {
    c->close_after_flush = 1;
    flush_client(c);
}

static int reserve(Client *c, size_t need) {
    if (need > MAX_OUTBOUND_BYTES) {
        /* Frames still queued for this client are lost. */
        log_info("fd %d exceeded outbound limit, disconnecting", c->fd);
        mark_dead(c);
        return -1;
    }
    if (need > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < need) cap *= 2;
        uint8_t *p = realloc(c->out, cap);
        if (!p) {
            mark_dead(c);
            return -1;
        }
        c->out = p;
        c->out_cap = cap;
    }
    return 0;
}

void out_append(Client *c, const uint8_t *data, size_t len) {
    if (!c || c->dead || c->fd < 0) return;
    if (reserve(c, c->out_len + len) < 0) return;
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    flush_client(c);
}

void send_frame(Client *c, uint8_t type, uint16_t seq, const uint8_t *payload, size_t len) {
    if (!c || c->dead || c->fd < 0) return;
    if (c->proto == PROTO_HTTP) return; /* not upgraded: no Tundra frames */
    if (len > TUNDRA_MAX_PAYLOAD) {
        log_info("refusing to send oversized payload (%zu bytes)", len);
        return;
    }
    uint8_t wshdr[10];
    size_t wslen = c->proto == PROTO_WS ? ws_header(wshdr, TUNDRA_HEADER_LEN + len) : 0;
    size_t need = c->out_len + wslen + TUNDRA_HEADER_LEN + len;
    if (reserve(c, need) < 0) return;
    if (wslen) memcpy(c->out + c->out_len, wshdr, wslen);
    size_t at = c->out_len + wslen;
    tundra_put_header(c->out + at, type, seq, len);
    if (len) memcpy(c->out + at + TUNDRA_HEADER_LEN, payload, len);
    c->out_len = need;
    if (type == MSG_CHAT && len >= 8) add_mark(c, need, tundra_get_u64(payload));
    if (c->session) c->session->messages_sent++;
    log_debug("-> fd %d %s seq %u len %zu", c->fd, tundra_type_name(type), seq, len);
    flush_client(c);
}

void push_frame(Client *c, uint8_t type, const uint8_t *payload, size_t len) {
    if (!c) return;
    uint16_t seq = c->session ? c->session->out_seq++ : 0;
    send_frame(c, type, seq, payload, len);
}

void send_msg(Client *c, const TundraMsg *msg, uint16_t seq, int push) {
    static uint8_t buf[TUNDRA_MAX_PAYLOAD];
    long n = tundra_encode(msg, buf, sizeof(buf));
    if (n < 0) {
        log_info("failed to encode %s", tundra_type_name(msg->type));
        return;
    }
    if (push) push_frame(c, msg->type, buf, (size_t)n);
    else send_frame(c, msg->type, seq, buf, (size_t)n);
}

static TundraBytes cbytes(const char *s) {
    size_t n = strlen(s);
    if (n > 256) n = 256;
    TundraBytes b = { (const uint8_t *)s, n };
    return b;
}

size_t build_error(uint8_t *buf, size_t cap, uint16_t code, const char *text) {
    TundraMsg m = { .type = MSG_ERROR };
    m.u.error.code = code;
    m.u.error.text = cbytes(text);
    long n = tundra_encode(&m, buf, cap);
    return n < 0 ? 0 : (size_t)n;
}

void send_error(Client *c, uint16_t seq, uint16_t code, const char *text) {
    uint8_t buf[4 + 256];
    size_t n = build_error(buf, sizeof(buf), code, text);
    if (n) send_frame(c, MSG_ERROR, seq, buf, n);
}

void send_system(Client *c, uint16_t seq, const uuid_t id, const char *text) {
    TundraMsg m = { .type = MSG_SYSTEM };
    memcpy(m.u.system.id, id, TUNDRA_UUID_LEN);
    m.u.system.text = cbytes(text);
    send_msg(c, &m, seq, 0);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */

void mark_dead(Client *c) {
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
            c->fd = fd;
            c->accepted_at = c->last_seen = mono_s();
            return c;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* input                                                               */

static void process_input(Client *c) {
    if (c->proto != PROTO_TCP) {
        ws_process_input(c);
        return;
    }
    size_t off = 0;
    while (!c->dead && !c->close_after_flush && c->in_len - off >= TUNDRA_HEADER_LEN) {
        size_t plen = tundra_get_u16(c->in + off + 4);
        if (c->in_len - off < TUNDRA_HEADER_LEN + plen) break;
        handle_frame(c, c->in + off, plen);
        off += TUNDRA_HEADER_LEN + plen;
    }
    if (off > 0) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

static void read_client(Client *c) {
    /* Bounded so one busy client cannot starve the rest; epoll is
     * level-triggered and reports the socket again if data remains. */
    for (int i = 0; i < MAX_READS_PER_EVENT && !c->dead && !c->close_after_flush; i++) {
        /* The buffer holds one maximum-size frame and complete frames are
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

static void reject_full(int fd) {
    uint8_t frame[TUNDRA_HEADER_LEN + 4 + 256];
    size_t len = build_error(frame + TUNDRA_HEADER_LEN, sizeof(frame) - TUNDRA_HEADER_LEN, ERR_SERVER_ERROR, "Server full");
    tundra_put_header(frame, MSG_ERROR, 0, len);
    ssize_t ignored = send(fd, frame, TUNDRA_HEADER_LEN + len, MSG_NOSIGNAL | MSG_DONTWAIT);
    (void)ignored;
    close(fd);
}

static void accept_clients(int listen_fd, int proto) {
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) log_info("accept failed: %s", strerror(errno));
            return;
        }
        Client *c = alloc_client(fd);
        if (!c) {
            log_info("no free client slots, rejecting fd %d", fd);
            if (proto == PROTO_TCP) reject_full(fd);
            else close(fd);
            continue;
        }
        c->proto = proto;
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
/* setup and loop                                                      */

static int open_listener(int port, void *marker) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 128) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = marker;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl");
        close(fd);
        return -1;
    }
    return fd;
}

int net_listen(int port) {
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }
    server_fd = open_listener(port, &listen_marker);
    return server_fd < 0 ? -1 : 0;
}

int net_listen_ws(int port) {
    ws_fd = open_listener(port, &ws_listen_marker);
    return ws_fd < 0 ? -1 : 0;
}

void net_run(void (*tick)(void)) {
    struct epoll_event events[MAX_EVENTS];
    time_t last_tick = mono_s();

    while (running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0 && errno != EINTR) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == &listen_marker) {
                accept_clients(server_fd, PROTO_TCP);
                continue;
            }
            if (events[i].data.ptr == &ws_listen_marker) {
                accept_clients(ws_fd, PROTO_HTTP);
                continue;
            }
            Client *c = events[i].data.ptr;
            if (c->fd < 0 || c->dead) continue;
            if (events[i].events & EPOLLIN) read_client(c);
            if (!c->dead && (events[i].events & EPOLLOUT)) flush_client(c);
            if (!c->dead && (events[i].events & (EPOLLERR | EPOLLHUP))) mark_dead(c);
        }

        time_t now = mono_s();
        if (now != last_tick) {
            tick();
            last_tick = now;
        }

        /* Close sockets only after the whole batch is handled, so a reused
         * fd can never be confused with a client closed in this batch. */
        reap_dead_clients();
    }
}

void net_shutdown(void) {
    log_info("shutting down");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) {
            mark_dead(&clients[i]);
            reap_client(&clients[i]);
        }
    }
    if (epoll_fd >= 0) close(epoll_fd);
    if (server_fd >= 0) close(server_fd);
    if (ws_fd >= 0) close(ws_fd);
}
