#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* arc4random_buf and BSD socket options under -std=c11 */
#elif !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "tundra_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && !defined(TUNDRA_POLLER_POLL)
#define POLLER_EPOLL 1
#elif (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)) && \
    !defined(TUNDRA_POLLER_POLL)
#define POLLER_KQUEUE 1
#else
#define POLLER_POLL 1
#endif

#ifdef _WIN32
#include <bcrypt.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

#if POLLER_EPOLL
#include <sys/epoll.h>
#endif
#if POLLER_KQUEUE
#include <sys/event.h>
#include <sys/types.h>
#endif
#if defined(__linux__)
#include <sys/random.h>
#endif

/* ------------------------------------------------------------------ */
/* process and signals                                                 */

static void (*stop_cb)(void);
static void (*reload_cb)(void);

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        if (stop_cb) stop_cb();
        return TRUE;
    }
    return FALSE;
}

int plat_init(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}

void plat_cleanup(void) {
    WSACleanup();
}

void plat_handle_signals(void (*on_stop)(void), void (*on_reload)(void)) {
    stop_cb = on_stop;
    reload_cb = on_reload; /* no SIGHUP on Windows */
    SetConsoleCtrlHandler(console_handler, TRUE);
}
#else
static void on_signal(int sig) {
    if (sig == SIGHUP) {
        if (reload_cb) reload_cb();
    } else if (stop_cb) {
        stop_cb();
    }
}

int plat_init(void) {
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void plat_cleanup(void) {}

void plat_handle_signals(void (*on_stop)(void), void (*on_reload)(void)) {
    stop_cb = on_stop;
    reload_cb = on_reload;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    if (on_reload) sigaction(SIGHUP, &sa, NULL);
}
#endif

/* ------------------------------------------------------------------ */
/* sockets                                                             */

static int set_nonblocking(tsock_t s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

static void no_sigpipe(tsock_t s) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)s;
#endif
}

void plat_close(tsock_t s) {
    if (s == TSOCK_INVALID) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

tsock_t plat_listen(int port) {
    tsock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == TSOCK_INVALID) return TSOCK_INVALID;
#ifdef _WIN32
    /* exclusive address use: no other process can steal the port */
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));
#else
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s, 128) != 0 || set_nonblocking(s) != 0) {
        plat_close(s);
        return TSOCK_INVALID;
    }
    return s;
}

tsock_t plat_accept(tsock_t listener) {
    for (;;) {
        tsock_t s = accept(listener, NULL, NULL);
        if (s == TSOCK_INVALID) {
            if (plat_interrupted()) continue;
            return TSOCK_INVALID;
        }
        if (set_nonblocking(s) != 0) {
            plat_close(s);
            continue;
        }
        no_sigpipe(s);
        return s;
    }
}

tsock_t plat_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return TSOCK_INVALID;
    tsock_t s = TSOCK_INVALID;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == TSOCK_INVALID) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        plat_close(s);
        s = TSOCK_INVALID;
    }
    freeaddrinfo(res);
    if (s != TSOCK_INVALID) no_sigpipe(s);
    return s;
}

long plat_send(tsock_t s, const void *buf, size_t len) {
#ifdef _WIN32
    if (len > 0x7fffffff) len = 0x7fffffff;
    return send(s, (const char *)buf, (int)len, 0);
#elif defined(MSG_NOSIGNAL)
    return (long)send(s, buf, len, MSG_NOSIGNAL);
#else
    return (long)send(s, buf, len, 0); /* SO_NOSIGPIPE set on the socket */
#endif
}

long plat_recv(tsock_t s, void *buf, size_t len) {
#ifdef _WIN32
    if (len > 0x7fffffff) len = 0x7fffffff;
    return recv(s, (char *)buf, (int)len, 0);
#else
    return (long)recv(s, buf, len, 0);
#endif
}

int plat_would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int plat_interrupted(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

const char *plat_socket_error(void) {
#ifdef _WIN32
    static char buf[32];
    snprintf(buf, sizeof(buf), "socket error %d", WSAGetLastError());
    return buf;
#else
    return strerror(errno);
#endif
}

int plat_wait_readable(tsock_t s, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD pfd;
    pfd.fd = s;
    pfd.events = POLLRDNORM;
    pfd.revents = 0;
    int r = WSAPoll(&pfd, 1, timeout_ms);
#else
    struct pollfd pfd = { s, POLLIN, 0 };
    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0 && errno == EINTR) return 0;
#endif
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* poller                                                              */

#if POLLER_EPOLL

struct Poller {
    int fd;
};

Poller *poller_create(void) {
    Poller *p = malloc(sizeof(*p));
    if (!p) return NULL;
    p->fd = epoll_create1(EPOLL_CLOEXEC);
    if (p->fd < 0) {
        free(p);
        return NULL;
    }
    return p;
}

void poller_free(Poller *p) {
    if (!p) return;
    close(p->fd);
    free(p);
}

static int epoll_set(Poller *p, int op, tsock_t s, void *ptr, int write) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | (write ? EPOLLOUT : 0);
    ev.data.ptr = ptr;
    return epoll_ctl(p->fd, op, s, &ev);
}

int poller_add(Poller *p, tsock_t s, void *ptr) {
    return epoll_set(p, EPOLL_CTL_ADD, s, ptr, 0);
}

int poller_set_write(Poller *p, tsock_t s, void *ptr, int on) {
    return epoll_set(p, EPOLL_CTL_MOD, s, ptr, on);
}

void poller_remove(Poller *p, tsock_t s) {
    epoll_ctl(p->fd, EPOLL_CTL_DEL, s, NULL);
}

int poller_wait(Poller *p, PollEvent *out, int max, int timeout_ms) {
    struct epoll_event evs[64];
    if (max > 64) max = 64;
    int n = epoll_wait(p->fd, evs, max, timeout_ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) {
        out[i].ptr = evs[i].data.ptr;
        out[i].readable = (evs[i].events & EPOLLIN) != 0;
        out[i].writable = (evs[i].events & EPOLLOUT) != 0;
        out[i].error = (evs[i].events & (EPOLLERR | EPOLLHUP)) != 0;
    }
    return n;
}

const char *poller_backend(void) {
    return "epoll";
}

#elif POLLER_KQUEUE

struct Poller {
    int kq;
};

Poller *poller_create(void) {
    Poller *p = malloc(sizeof(*p));
    if (!p) return NULL;
    p->kq = kqueue();
    if (p->kq < 0) {
        free(p);
        return NULL;
    }
    fcntl(p->kq, F_SETFD, FD_CLOEXEC);
    return p;
}

void poller_free(Poller *p) {
    if (!p) return;
    close(p->kq);
    free(p);
}

int poller_add(Poller *p, tsock_t s, void *ptr) {
    struct kevent ev;
    EV_SET(&ev, s, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, ptr);
    return kevent(p->kq, &ev, 1, NULL, 0, NULL);
}

int poller_set_write(Poller *p, tsock_t s, void *ptr, int on) {
    struct kevent ev;
    EV_SET(&ev, s, EVFILT_WRITE, on ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, ptr);
    int r = kevent(p->kq, &ev, 1, NULL, 0, NULL);
    return (r < 0 && !on && errno == ENOENT) ? 0 : r;
}

void poller_remove(Poller *p, tsock_t s) {
    struct kevent ev[2];
    EV_SET(&ev[0], s, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[1], s, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    /* the write filter may not be registered; errors are expected */
    kevent(p->kq, &ev[0], 1, NULL, 0, NULL);
    kevent(p->kq, &ev[1], 1, NULL, 0, NULL);
}

int poller_wait(Poller *p, PollEvent *out, int max, int timeout_ms) {
    struct kevent evs[64];
    if (max > 64) max = 64;
    struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
    int n = kevent(p->kq, NULL, 0, evs, max, &ts);
    if (n < 0) return errno == EINTR ? 0 : -1;
    /* kqueue reports read and write readiness as separate events; that is
     * fine for the caller, which handles each kind independently */
    for (int i = 0; i < n; i++) {
        out[i].ptr = evs[i].udata;
        out[i].readable = evs[i].filter == EVFILT_READ;
        out[i].writable = evs[i].filter == EVFILT_WRITE;
        out[i].error = (evs[i].flags & EV_ERROR) != 0 ||
                       ((evs[i].flags & EV_EOF) && evs[i].filter == EVFILT_WRITE);
    }
    return n;
}

const char *poller_backend(void) {
    return "kqueue";
}

#else /* POLLER_POLL: poll() on POSIX, WSAPoll on Windows */

#ifdef _WIN32
typedef WSAPOLLFD tpollfd;
#define TPOLL(fds, n, t) WSAPoll((fds), (ULONG)(n), (t))
#define TPOLL_IN POLLRDNORM
#define TPOLL_OUT POLLWRNORM
#else
typedef struct pollfd tpollfd;
#define TPOLL(fds, n, t) poll((fds), (nfds_t)(n), (t))
#define TPOLL_IN POLLIN
#define TPOLL_OUT POLLOUT
#endif

struct Poller {
    tpollfd *fds;
    void **ptrs;
    size_t count;
    size_t cap;
};

Poller *poller_create(void) {
    Poller *p = calloc(1, sizeof(*p));
    return p;
}

void poller_free(Poller *p) {
    if (!p) return;
    free(p->fds);
    free(p->ptrs);
    free(p);
}

static long find(Poller *p, tsock_t s) {
    for (size_t i = 0; i < p->count; i++) {
        if (p->fds[i].fd == s) return (long)i;
    }
    return -1;
}

int poller_add(Poller *p, tsock_t s, void *ptr) {
    if (p->count == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 64;
        tpollfd *fds = realloc(p->fds, cap * sizeof(*fds));
        if (!fds) return -1;
        p->fds = fds;
        void **ptrs = realloc(p->ptrs, cap * sizeof(*ptrs));
        if (!ptrs) return -1;
        p->ptrs = ptrs;
        p->cap = cap;
    }
    p->fds[p->count].fd = s;
    p->fds[p->count].events = TPOLL_IN;
    p->fds[p->count].revents = 0;
    p->ptrs[p->count] = ptr;
    p->count++;
    return 0;
}

int poller_set_write(Poller *p, tsock_t s, void *ptr, int on) {
    long i = find(p, s);
    if (i < 0) return -1;
    p->fds[i].events = (short)(TPOLL_IN | (on ? TPOLL_OUT : 0));
    p->ptrs[i] = ptr;
    return 0;
}

void poller_remove(Poller *p, tsock_t s) {
    long i = find(p, s);
    if (i < 0) return;
    p->count--;
    p->fds[i] = p->fds[p->count];
    p->ptrs[i] = p->ptrs[p->count];
}

int poller_wait(Poller *p, PollEvent *out, int max, int timeout_ms) {
    if (p->count == 0) {
#ifdef _WIN32
        Sleep((DWORD)timeout_ms);
#else
        poll(NULL, 0, timeout_ms);
#endif
        return 0;
    }
    int r = TPOLL(p->fds, p->count, timeout_ms);
    if (r < 0) {
#ifdef _WIN32
        return -1;
#else
        return errno == EINTR ? 0 : -1;
#endif
    }
    int n = 0;
    for (size_t i = 0; i < p->count && n < max; i++) {
        short re = p->fds[i].revents;
        if (!re) continue;
        out[n].ptr = p->ptrs[i];
        out[n].readable = (re & (TPOLL_IN | POLLHUP)) != 0;
        out[n].writable = (re & TPOLL_OUT) != 0;
        out[n].error = (re & (POLLERR | POLLNVAL)) != 0;
        n++;
    }
    return n;
}

const char *poller_backend(void) {
#ifdef _WIN32
    return "WSAPoll";
#else
    return "poll";
#endif
}

#endif

/* ------------------------------------------------------------------ */
/* time and randomness                                                 */

#ifdef _WIN32
uint64_t plat_wall_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    return t / 10000u - 11644473600000ULL;
}

uint64_t plat_mono_ms(void) {
    return (uint64_t)GetTickCount64();
}

int plat_random(void *buf, size_t len) {
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}
#else
static uint64_t clock_ms(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t plat_wall_ms(void) {
    return clock_ms(CLOCK_REALTIME);
}

uint64_t plat_mono_ms(void) {
    return clock_ms(CLOCK_MONOTONIC);
}

int plat_random(void *buf, size_t len) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(buf, len);
    return 0;
#else
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
#endif
}
#endif

/* ------------------------------------------------------------------ */
/* UUIDs                                                               */

void tundra_uuid_generate(tundra_uuid_t out) {
    if (plat_random(out, 16) != 0) {
        /* no secure randomness: refuse to hand out guessable session ids */
        fprintf(stderr, "fatal: no secure random source\n");
        abort();
    }
    out[6] = (uint8_t)((out[6] & 0x0f) | 0x40); /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3f) | 0x80); /* RFC 4122 variant */
}

int tundra_uuid_is_null(const tundra_uuid_t u) {
    for (int i = 0; i < 16; i++) {
        if (u[i]) return 0;
    }
    return 1;
}

void tundra_uuid_unparse(const tundra_uuid_t u, char out[37]) {
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[u[i] >> 4];
        out[o++] = hex[u[i] & 15];
    }
    out[o] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int tundra_uuid_parse(const char *s, tundra_uuid_t out) {
    if (strlen(s) != 36) return -1;
    int b = 0;
    for (int i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return -1;
            i++;
            continue;
        }
        int hi = hexval(s[i]), lo = hexval(s[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[b++] = (uint8_t)(hi << 4 | lo);
        i += 2;
    }
    return 0;
}
