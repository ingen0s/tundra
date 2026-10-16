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
static volatile sig_atomic_t running = 1;
static int listen_marker; /* epoll data.ptr for the listening socket */

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
    }
    if (c->dead) return;
    set_epollout(c, c->out_len > 0);
    if (c->out_len == 0 && c->close_after_flush) mark_dead(c);
}

void close_after_flush(Client *c) {
    c->close_after_flush = 1;
    flush_client(c);
}

void send_frame(Client *c, uint8_t type, uint16_t seq, const uint8_t *payload, size_t len) {
    if (!c || c->dead || c->fd < 0) return;
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

void send_error(Client *c, uint16_t seq, uint16_t code, const char *text) {
    TundraMsg m = { .type = MSG_ERROR };
    m.u.error.code = code;
    m.u.error.text = cbytes(text);
    send_msg(c, &m, seq, 0);
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
            c->last_seen = mono_s();
            return c;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* input                                                               */

static void process_input(Client *c) {
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
    while (!c->dead && !c->close_after_flush) {
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
/* setup and loop                                                      */

int net_listen(int port) {
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }
    if (listen(server_fd, 128) < 0) {
        perror("listen");
        return -1;
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.ptr = &listen_marker;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl");
        return -1;
    }
    return 0;
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
                accept_clients();
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
}
