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
 *   -G caps       advertise as an agent with these capabilities
 *   -q agent:body send an agent request (request ids count up from 1)
 *   -Q            list agents
 * Options:
 *   -f id         with -r: resume from this message id instead of the server's watermark
 *   -a            acknowledge every CHAT received
 *   -k token      authentication token for this username
 *   -s            connect with TLS (verifies the certificate against system CAs)
 *   -x            with -s, skip certificate verification (self-signed test setups)
 *   -E            act as an echo agent: answer every request with "echo: <body>"
 *   -S            with -E, stream: send a partial "working" response first
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
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tundra_platform.h"

#ifdef TUNDRA_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
static SSL_CTX *tls_ctx = NULL;
static SSL *tls = NULL;
#endif

#include "tundra_codec.h"

#define MAX_ACTIONS 128

typedef struct {
    char op;
    const char *arg;
    const char *target; /* for 'm' */
} Action;

static tsock_t sock = TSOCK_INVALID;
static int auto_ack = 0;
static int echo_agent = 0;
static int use_tls = 0;
static int insecure = 0;
static int stream_agent = 0;
static uint64_t next_req_id = 1;
static uint16_t next_seq = 0;
static uint8_t inbuf[TUNDRA_MAX_FRAME];
static size_t inlen = 0;

static long now_ms(void) {
    return (long)plat_mono_ms();
}

static TundraBytes str(const char *s) {
    TundraBytes b = { (const uint8_t *)s, strlen(s) };
    return b;
}

static int io_tls(void) {
#ifdef TUNDRA_TLS
    return tls != NULL;
#else
    return 0;
#endif
}

static long io_send(const void *buf, size_t len) {
#ifdef TUNDRA_TLS
    if (tls) {
        int r = SSL_write(tls, buf, (int)(len > 0x7fffffff ? 0x7fffffff : len));
        return r > 0 ? r : -1;
    }
#endif
    return plat_send(sock, buf, len);
}

static long io_recv(void *buf, size_t len) {
#ifdef TUNDRA_TLS
    if (tls) {
        int r = SSL_read(tls, buf, (int)(len > 0x7fffffff ? 0x7fffffff : len));
        if (r > 0) return r;
        int e = SSL_get_error(tls, r);
        return (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && r == 0)) ? 0 : -1;
    }
#endif
    return plat_recv(sock, buf, len);
}

static int io_pending(void) {
#ifdef TUNDRA_TLS
    if (tls) return SSL_pending(tls) > 0;
#endif
    return 0;
}

static int send_all(const uint8_t *buf, size_t len) {
    while (len > 0) {
        long n = io_send(buf, len);
        if (n < 0) {
            if (!io_tls() && plat_interrupted()) continue;
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
            if (!tundra_uuid_is_null(m.u.system.id)) {
                char s[37];
                tundra_uuid_unparse(m.u.system.id, s);
                printf("SESSION %s\n", s);
            }
            printf("SYSTEM %.*s\n", (int)m.u.system.text.len, (const char *)m.u.system.text.p);
            break;
        case MSG_CHAT:
            print_chat("CHAT", &m);
            if (auto_ack) {
                TundraMsg ack;
                memset(&ack, 0, sizeof(ack));
                ack.type = MSG_ACK;
                ack.u.sent.id = m.u.chat.id;
                send_msg(&ack);
            }
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
        case MSG_AGENT_REQUEST_IN: {
            printf("AREQ %.*s %llu %.*s\n", (int)m.u.agent_req.peer.len, (const char *)m.u.agent_req.peer.p,
                   (unsigned long long)m.u.agent_req.req_id, (int)m.u.agent_req.body.len,
                   (const char *)m.u.agent_req.body.p);
            if (echo_agent) {
                TundraMsg r;
                memset(&r, 0, sizeof(r));
                r.type = MSG_AGENT_RESPONSE;
                r.u.agent_resp.peer = m.u.agent_req.peer;
                r.u.agent_resp.req_id = m.u.agent_req.req_id;
                r.u.agent_resp.status = AGENT_OK;
                if (stream_agent) {
                    r.u.agent_resp.final = 0;
                    r.u.agent_resp.body = str("working");
                    send_msg(&r);
                }
                static char reply[TUNDRA_MAX_TEXT];
                int n = snprintf(reply, sizeof(reply), "echo: %.*s", (int)m.u.agent_req.body.len,
                                 (const char *)m.u.agent_req.body.p);
                r.u.agent_resp.final = 1;
                r.u.agent_resp.body.p = (const uint8_t *)reply;
                r.u.agent_resp.body.len = n < 0 ? 0 : (size_t)n;
                send_msg(&r);
            }
            break;
        }
        case MSG_AGENT_RESPONSE_IN:
            printf("ARESP %.*s %llu %u %u %.*s\n", (int)m.u.agent_resp.peer.len, (const char *)m.u.agent_resp.peer.p,
                   (unsigned long long)m.u.agent_resp.req_id, m.u.agent_resp.status, m.u.agent_resp.final,
                   (int)m.u.agent_resp.body.len, (const char *)m.u.agent_resp.body.p);
            break;
        case MSG_AGENT_LIST: {
            TundraReader it = tundra_reader(m.u.list.entries.p, m.u.list.entries.len);
            TundraBytes name, caps;
            uint8_t online;
            printf("AGENTS %u%s\n", m.u.list.count, m.u.list.truncated ? " truncated" : "");
            while (tundra_next_agent(&it, &name, &online, &caps)) {
                printf("AGENTINFO %.*s %u %.*s\n", (int)name.len, (const char *)name.p, online, (int)caps.len,
                       (const char *)caps.p);
            }
            break;
        }
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
        int pr = io_pending() ? 1 : plat_wait_readable(sock, (int)wait);
        if (pr < 0) return -1;
        if (pr == 0) return 0;
        long n = io_recv(inbuf + inlen, sizeof(inbuf) - inlen);
        if (n == 0) {
            printf("CLOSED\n");
            fflush(stdout);
            return -1;
        }
        if (n < 0) {
            if (plat_interrupted()) continue;
            return -1;
        }
        inlen += (size_t)n;
    }
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
        case 'Q': m.type = MSG_LIST_AGENTS; break;
        case 'G':
            m.type = MSG_AGENT_ADVERTISE;
            m.u.advertise.capabilities = str(a->arg);
            break;
        case 'q': {
            const char *colon = strchr(a->arg, ':');
            if (!colon) {
                fprintf(stderr, "-q expects agent:body\n");
                return 0;
            }
            m.type = MSG_AGENT_REQUEST;
            m.u.agent_req.peer.p = (const uint8_t *)a->arg;
            m.u.agent_req.peer.len = (size_t)(colon - a->arg);
            m.u.agent_req.req_id = next_req_id++;
            m.u.agent_req.body = str(colon + 1);
            break;
        }
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

#ifndef TUNDRA_RELEASE
#define TUNDRA_RELEASE "dev"
#endif

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s -u username [-H host] [-p port]"
#ifdef TUNDRA_TLS
            " [-s [-x]]"
#endif
            " [-k token] [-r session-id [-f last-id]] [-a]\n"
            "          [-j #room] [-L #room] [-t target] [-m text] [-R] [-W #room] [-y #room] [-c]\n"
            "          [-G capabilities] [-q agent:body] [-Q] [-E [-S]]\n"
            "          [-w seconds] [-V]\n",
            prog);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    const char *port = "4242";
    const char *user = NULL;
    const char *resume = NULL;
    const char *resume_from = NULL;
    const char *token = NULL;
    const char *target = "#lobby";
    Action actions[MAX_ACTIONS];
    int nactions = 0;
    double wait_s = 1.0;

    int opt;
    while ((opt = getopt(argc, argv, "H:p:u:r:f:ak:ESsxj:L:t:m:RW:y:cG:q:Qw:Vh")) != -1) {
        switch (opt) {
            case 'H': host = optarg; break;
            case 'p': port = optarg; break;
            case 'u': user = optarg; break;
            case 'r': resume = optarg; break;
            case 'f': resume_from = optarg; break;
            case 'a': auto_ack = 1; break;
            case 'k': token = optarg; break;
            case 'E': echo_agent = 1; break;
            case 's': use_tls = 1; break;
            case 'x': insecure = 1; break;
            case 'S': stream_agent = 1; break;
            case 't': target = optarg; break;
            case 'w': wait_s = atof(optarg); break;
            case 'V': printf("tundra_client %s\n", TUNDRA_RELEASE); return 0;
            case 'j': case 'L': case 'm': case 'R': case 'W': case 'y': case 'c': case 'G': case 'q': case 'Q':
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
    if (resume && tundra_uuid_parse(resume, reg.u.reg.id) != 0) {
        fprintf(stderr, "invalid session id: %s\n", resume);
        return 2;
    }
    uint8_t ext[512];
    TundraWriter xw = tundra_writer(ext, sizeof(ext));
    if (resume_from) {
        uint8_t v[8];
        tundra_put_u64(v, strtoull(resume_from, NULL, 10));
        tundra_wr_tlv(&xw, REG_RESUME_FROM, v, sizeof(v));
    }
    if (token) tundra_wr_tlv(&xw, REG_TOKEN, token, strlen(token));
    if (!xw.ok) {
        fprintf(stderr, "token too long\n");
        return 2;
    }
    reg.u.reg.ext.p = ext;
    reg.u.reg.ext.len = xw.len;

    if (plat_init() != 0) {
        fprintf(stderr, "cannot start networking\n");
        return 1;
    }
    sock = plat_connect(host, port);
    if (sock == TSOCK_INVALID) {
        fprintf(stderr, "could not connect to %s:%s\n", host, port);
        return 1;
    }
    if (use_tls) {
#ifdef TUNDRA_TLS
        tls_ctx = SSL_CTX_new(TLS_client_method());
        if (!tls_ctx) {
            fprintf(stderr, "cannot create a TLS context\n");
            return 1;
        }
        SSL_CTX_set_min_proto_version(tls_ctx, TLS1_2_VERSION);
        if (insecure) {
            SSL_CTX_set_verify(tls_ctx, SSL_VERIFY_NONE, NULL);
        } else {
            /* SSL_CERT_FILE / SSL_CERT_DIR override the built-in locations */
            SSL_CTX_set_default_verify_paths(tls_ctx);
#if defined(_WIN32) && OPENSSL_VERSION_NUMBER >= 0x30200000L
            /* the Windows certificate store: a static OpenSSL has no
             * usable default CA path on a user's machine */
            SSL_CTX_load_verify_store(tls_ctx, "org.openssl.winstore://");
#endif
            SSL_CTX_set_verify(tls_ctx, SSL_VERIFY_PEER, NULL);
        }
        tls = SSL_new(tls_ctx);
        if (!tls || SSL_set_fd(tls, (int)sock) != 1) {
            fprintf(stderr, "cannot set up TLS on the connection\n");
            return 1;
        }
        SSL_set_tlsext_host_name(tls, host);
        if (!insecure) SSL_set1_host(tls, host);
        if (SSL_connect(tls) != 1) {
            char err[256];
            ERR_error_string_n(ERR_get_error(), err, sizeof(err));
            fprintf(stderr, "TLS handshake failed: %s\n", err);
            return 1;
        }
#else
        fprintf(stderr, "this build has no TLS support\n");
        return 2;
#endif
    }

    if (send_msg(&reg) < 0) {
        fprintf(stderr, "register failed\n");
        return 1;
    }
    int t = pump(now_ms() + 5000, MSG_SYSTEM);
    if (t != MSG_SYSTEM) {
        if (t == 0) fprintf(stderr, "no reply to register\n");
        plat_close(sock);
        return 1;
    }

    for (int i = 0; i < nactions; i++) {
        if (run_action(&actions[i]) < 0) {
            fprintf(stderr, "send failed\n");
            return 1;
        }
    }

    pump(now_ms() + (long)(wait_s * 1000), -1);
    plat_close(sock);
    return 0;
}
