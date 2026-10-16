/*
 * Thin platform layer so the server and test client build natively on
 * Linux, macOS (and other BSDs) and Windows.
 *
 * Sockets: non-blocking TCP with no SIGPIPE, behind `tsock_t`.
 * Poller:  epoll on Linux, kqueue on macOS/BSD, poll()/WSAPoll elsewhere.
 * Also:    clocks, secure random bytes, UUIDs, and shutdown/reload signals.
 *
 * Define TUNDRA_POLLER_POLL to force the portable poll() backend (useful
 * for testing it on Linux).
 */
#ifndef TUNDRA_PLATFORM_H
#define TUNDRA_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET tsock_t;
#define TSOCK_INVALID INVALID_SOCKET
#else
typedef int tsock_t;
#define TSOCK_INVALID (-1)
#endif

/* ---- process ---- */

/* Starts the socket library (Windows) and ignores SIGPIPE (POSIX). */
int plat_init(void);
void plat_cleanup(void);

/* `on_stop` runs on Ctrl+C / SIGINT / SIGTERM, `on_reload` on SIGHUP
 * (POSIX only). Both are called from a signal or console handler, so they
 * must only set a flag. */
void plat_handle_signals(void (*on_stop)(void), void (*on_reload)(void));

/* ---- sockets ---- */

/* A non-blocking listening socket on all IPv4 interfaces. */
tsock_t plat_listen(int port);
/* Accepts one pending connection as a non-blocking socket, or returns
 * TSOCK_INVALID (check plat_would_block() to tell "none pending" from an
 * error). */
tsock_t plat_accept(tsock_t listener);
/* Blocking connect to host:port (client side). */
tsock_t plat_connect(const char *host, const char *port);
long plat_send(tsock_t s, const void *buf, size_t len);   /* never raises SIGPIPE */
long plat_recv(tsock_t s, void *buf, size_t len);
void plat_close(tsock_t s);
int plat_would_block(void);    /* last socket error was EAGAIN/EWOULDBLOCK */
int plat_interrupted(void);    /* last socket error was EINTR */
const char *plat_socket_error(void);
/* Waits up to timeout_ms for `s` to become readable: 1 ready, 0 timeout, -1 error. */
int plat_wait_readable(tsock_t s, int timeout_ms);

/* ---- poller ---- */

typedef struct Poller Poller;
typedef struct {
    void *ptr;
    int readable;
    int writable;
    int error;      /* hang-up or socket error */
} PollEvent;

Poller *poller_create(void);
void poller_free(Poller *p);
int poller_add(Poller *p, tsock_t s, void *ptr);              /* watches readability */
int poller_set_write(Poller *p, tsock_t s, void *ptr, int on);/* also watch writability */
void poller_remove(Poller *p, tsock_t s);
/* Returns the number of events written to `out` (0 on timeout), or -1. */
int poller_wait(Poller *p, PollEvent *out, int max, int timeout_ms);
const char *poller_backend(void);

/* ---- time and randomness ---- */

uint64_t plat_wall_ms(void);   /* milliseconds since the Unix epoch */
uint64_t plat_mono_ms(void);   /* monotonic milliseconds */
int plat_random(void *buf, size_t len); /* cryptographically secure; 0 on success */

/* ---- UUIDs (random, version 4) ---- */

typedef uint8_t tundra_uuid_t[16];
void tundra_uuid_generate(tundra_uuid_t out);
int tundra_uuid_is_null(const tundra_uuid_t u);
void tundra_uuid_unparse(const tundra_uuid_t u, char out[37]);
int tundra_uuid_parse(const char *s, tundra_uuid_t out); /* 0 on success */

#endif /* TUNDRA_PLATFORM_H */
