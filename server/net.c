#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

#define MAX_EVENTS 64

Client clients[MAX_CLIENTS];

static Poller *poller = NULL;
static tsock_t server_sock = TSOCK_INVALID;
static tsock_t ws_sock = TSOCK_INVALID;
/* set from a signal handler (POSIX) or a console-handler thread (Windows) */
static atomic_int running = 1;
static int listen_marker;    /* poller ptr for the Tundra listener */
static int ws_listen_marker; /* poller ptr for the HTTP/WebSocket listener */

static void on_stop(void) {
    running = 0;
}

/* ------------------------------------------------------------------ */
/* output                                                              */

static void set_want_write(Client *c, int on) {
    on = on || c->tls_wants_write;
    if (c->want_write == on || c->fd < 0) return;
    if (poller_set_write(poller, c->sock, c, on) == 0) c->want_write = on;
}

/* Socket I/O through TLS when the client has it. Both return >0 for bytes
 * moved, -1 for "try again when the socket is ready", -2 for an error;
 * client_recv also returns 0 when the peer closed. */
static long client_send(Client *c, const void *buf, size_t len) {
    if (c->ssl) return srv_tls_send(c, buf, len);
    for (;;) {
        long n = plat_send(c->sock, buf, len);
        if (n >= 0) return n;
        if (plat_interrupted()) continue;
        if (plat_would_block()) return -1;
        log_debug("send failed on client %d: %s", c->fd, plat_socket_error());
        return -2;
    }
}

static long client_recv(Client *c, void *buf, size_t len) {
    if (c->ssl) return srv_tls_recv(c, buf, len);
    for (;;) {
        long n = plat_recv(c->sock, buf, len);
        if (n >= 0) return n;
        if (plat_interrupted()) continue;
        return plat_would_block() ? -1 : -2;
    }
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
        long n = client_send(c, c->out + off, c->out_len - off);
        if (n == -1) break;
        if (n < 0) {
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
    set_want_write(c, c->out_len > 0);
    if (c->out_len == 0 && c->close_after_flush) mark_dead(c);
}

void close_after_flush(Client *c) {
    c->close_after_flush = 1;
    flush_client(c);
}

static int reserve(Client *c, size_t need) {
    if (need > MAX_OUTBOUND_BYTES) {
        /* Frames still queued for this client are lost. */
        log_info("client %d exceeded outbound limit, disconnecting", c->fd);
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
    log_debug("-> client %d %s seq %u len %zu", c->fd, tundra_type_name(type), seq, len);
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

void send_system(Client *c, uint16_t seq, const tundra_uuid_t id, const char *text) {
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
        poller_remove(poller, c->sock);
        srv_tls_detach(c);
        plat_close(c->sock);
        log_info("client %d closed", c->fd);
    }
    free(c->out);
    memset(c, 0, offsetof(Client, in));
    c->fd = -1;
    c->sock = TSOCK_INVALID;
}

static void reap_dead_clients(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && clients[i].dead) reap_client(&clients[i]);
    }
}

static Client *alloc_client(tsock_t sock) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            Client *c = &clients[i];
            memset(c, 0, offsetof(Client, in));
            c->fd = i;
            c->sock = sock;
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
        long n = client_recv(c, c->in + c->in_len, sizeof(c->in) - c->in_len);
        if (n > 0) {
            c->in_len += (size_t)n;
            process_input(c);
            continue;
        }
        if (n != -1) {
            mark_dead(c); /* closed or failed */
            return;
        }
        /* TLS may need the socket writable to make progress on a read
         * (handshake or post-handshake messages) even with nothing queued */
        if (c->ssl) set_want_write(c, c->out_len > 0);
        return;
    }
}

static void reject_full(tsock_t s) {
    uint8_t frame[TUNDRA_HEADER_LEN + 4 + 256];
    size_t len = build_error(frame + TUNDRA_HEADER_LEN, sizeof(frame) - TUNDRA_HEADER_LEN, ERR_SERVER_ERROR, "Server full");
    tundra_put_header(frame, MSG_ERROR, 0, len);
    (void)plat_send(s, frame, TUNDRA_HEADER_LEN + len); /* best effort: the socket is non-blocking */
    plat_close(s);
}

static void accept_clients(tsock_t listener, int proto) {
    for (;;) {
        tsock_t s = plat_accept(listener);
        if (s == TSOCK_INVALID) {
            if (!plat_would_block()) log_info("accept failed: %s", plat_socket_error());
            return;
        }
        Client *c = alloc_client(s);
        if (!c) {
            log_info("no free client slots, rejecting a connection");
            if (proto == PROTO_TCP && !srv_tls_enabled()) reject_full(s); /* a plaintext error means nothing to a TLS client */
            else plat_close(s);
            continue;
        }
        c->proto = proto;
        if (srv_tls_attach(c) < 0 || poller_add(poller, s, c) < 0) {
            log_info("cannot set up client %d", c->fd);
            srv_tls_detach(c);
            plat_close(s);
            c->fd = -1;
            c->sock = TSOCK_INVALID;
            continue;
        }
        log_info("client %d connected", c->fd);
    }
}

/* ------------------------------------------------------------------ */
/* setup and loop                                                      */

static tsock_t open_listener(int port, void *marker) {
    tsock_t s = plat_listen(port);
    if (s == TSOCK_INVALID) {
        log_info("cannot listen on port %d: %s", port, plat_socket_error());
        return TSOCK_INVALID;
    }
    if (poller_add(poller, s, marker) < 0) {
        log_info("poller add failed: %s", plat_socket_error());
        plat_close(s);
        return TSOCK_INVALID;
    }
    return s;
}

int net_listen(int port) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].sock = TSOCK_INVALID;
    }
    if (plat_init() != 0) {
        log_info("cannot start networking");
        return -1;
    }
    poller = poller_create();
    if (!poller) {
        log_info("cannot create the %s poller", poller_backend());
        return -1;
    }
    server_sock = open_listener(port, &listen_marker);
    return server_sock == TSOCK_INVALID ? -1 : 0;
}

int net_listen_ws(int port) {
    ws_sock = open_listener(port, &ws_listen_marker);
    return ws_sock == TSOCK_INVALID ? -1 : 0;
}

void net_handle_signals(void (*on_reload)(void)) {
    plat_handle_signals(on_stop, on_reload);
}

/* TLS can hold decrypted bytes that no socket event will announce: give
 * those clients another read. Returns 1 if any still have more. */
static int drain_tls_pending(void) {
    int more = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd < 0 || c->dead || c->close_after_flush || !srv_tls_pending(c)) continue;
        read_client(c);
        if (!c->dead && c->out_len > 0) flush_client(c);
        /* a client closing after its flush reads no more, so its leftover
         * input must not keep the loop spinning */
        if (!c->dead && !c->close_after_flush && srv_tls_pending(c)) more = 1;
    }
    return more;
}

void net_run(void (*tick)(void)) {
    PollEvent events[MAX_EVENTS];
    time_t last_tick = mono_s();
    int tls_more = 0;

    while (running) {
        int n = poller_wait(poller, events, MAX_EVENTS, tls_more ? 0 : 1000);
        if (n < 0) {
            log_info("%s wait failed: %s", poller_backend(), plat_socket_error());
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].ptr == &listen_marker) {
                accept_clients(server_sock, PROTO_TCP);
                continue;
            }
            if (events[i].ptr == &ws_listen_marker) {
                accept_clients(ws_sock, PROTO_HTTP);
                continue;
            }
            Client *c = events[i].ptr;
            if (c->fd < 0 || c->dead) continue;
            /* with TLS a read may need the socket writable (handshake, key
             * updates), so a writable event also retries the read */
            if (events[i].readable || (c->ssl && events[i].writable)) read_client(c);
            if (!c->dead && (events[i].writable || (c->ssl && c->out_len > 0))) flush_client(c);
            if (!c->dead && events[i].error) mark_dead(c);
        }
        tls_more = drain_tls_pending();

        time_t now = mono_s();
        if (now != last_tick) {
            tick();
            last_tick = now;
        }

        /* Close sockets only after the whole batch is handled, so a reused
         * socket can never be confused with a client closed in this batch. */
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
    plat_close(server_sock);
    plat_close(ws_sock);
    poller_free(poller);
    plat_cleanup();
}
