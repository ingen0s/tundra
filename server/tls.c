/*
 * Optional built-in TLS (OpenSSL), for both the Tundra port and the
 * HTTP/WebSocket port. Enabled at build time when OpenSSL is found
 * (TUNDRA_TLS) and at run time with -C cert.pem -K key.pem.
 *
 * Connections stay non-blocking: OpenSSL's "want read" and "want write"
 * results map onto the poller, and bytes OpenSSL has already decrypted
 * but not yet handed over are drained without waiting for new socket
 * readiness (see srv_tls_pending).
 */
#include <stdio.h>
#include <string.h>

#include "server.h"

#ifdef TUNDRA_TLS

#include <openssl/err.h>
#include <openssl/ssl.h>

static SSL_CTX *ctx = NULL;

static void log_ssl_errors(const char *what) {
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        log_info("tls: %s: %s", what, buf);
    }
}

int srv_tls_available(void) {
    return 1;
}

int srv_tls_enabled(void) {
    return ctx != NULL;
}

int srv_tls_init(const char *cert, const char *key) {
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        log_ssl_errors("context");
        return -1;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION); /* the default in OpenSSL 3; explicit for 1.1.1 */
#endif
    /* the output queue moves as it drains, and partial writes are fine */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
        log_ssl_errors("certificate");
        log_info("tls: cannot load certificate chain from %s", cert);
        srv_tls_free();
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(ctx) != 1) {
        log_ssl_errors("key");
        log_info("tls: cannot load a private key matching the certificate from %s", key);
        srv_tls_free();
        return -1;
    }
    log_info("tls: enabled with %s", cert);
    return 0;
}

void srv_tls_free(void) {
    if (ctx) SSL_CTX_free(ctx);
    ctx = NULL;
}

int srv_tls_attach(Client *c) {
    if (!ctx) return 0;
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        log_ssl_errors("new connection");
        return -1;
    }
    if (SSL_set_fd(ssl, (int)c->sock) != 1) {
        log_ssl_errors("set fd");
        SSL_free(ssl);
        return -1;
    }
    SSL_set_accept_state(ssl);
    c->ssl = ssl;
    return 0;
}

void srv_tls_detach(Client *c) {
    if (!c->ssl) return;
    SSL *ssl = c->ssl;
    if (!c->tls_failed) SSL_shutdown(ssl); /* best effort close_notify */
    SSL_free(ssl);
    c->ssl = NULL;
}

/* Maps an OpenSSL result to the poller: -1 means "try again when the
 * socket is ready", -2 a fatal error, 0 a clean close by the peer. */
static long map_result(Client *c, int r, const char *what) {
    SSL *ssl = c->ssl;
    int err = SSL_get_error(ssl, r);
    switch (err) {
        case SSL_ERROR_WANT_READ:
            c->tls_wants_write = 0;
            return -1;
        case SSL_ERROR_WANT_WRITE:
            c->tls_wants_write = 1;
            return -1;
        case SSL_ERROR_ZERO_RETURN:
            return 0;
        case SSL_ERROR_SYSCALL:
            ERR_clear_error();
            c->tls_failed = 1;
            return r == 0 ? 0 : -2; /* EOF without close_notify counts as a close */
        default:
            log_ssl_errors(what);
            c->tls_failed = 1;
            return -2;
    }
}

long srv_tls_recv(Client *c, void *buf, size_t len) {
    if (len > 0x7fffffff) len = 0x7fffffff;
    ERR_clear_error();
    int r = SSL_read(c->ssl, buf, (int)len);
    if (r > 0) {
        c->tls_wants_write = 0;
        return r;
    }
    return map_result(c, r, "read");
}

long srv_tls_send(Client *c, const void *buf, size_t len) {
    if (len > 0x7fffffff) len = 0x7fffffff;
    ERR_clear_error();
    int r = SSL_write(c->ssl, buf, (int)len);
    if (r > 0) {
        c->tls_wants_write = 0;
        return r;
    }
    long m = map_result(c, r, "write");
    return m == 0 ? -2 : m; /* a close while writing is an error for the writer */
}

int srv_tls_pending(const Client *c) {
    return c->ssl && SSL_pending(c->ssl) > 0;
}

#else /* built without OpenSSL */

int srv_tls_available(void) { return 0; }
int srv_tls_enabled(void) { return 0; }
int srv_tls_init(const char *cert, const char *key) {
    (void)cert;
    (void)key;
    log_info("tls: this build has no TLS support (rebuild with OpenSSL installed)");
    return -1;
}
void srv_tls_free(void) {}
int srv_tls_attach(Client *c) { (void)c; return 0; }
void srv_tls_detach(Client *c) { (void)c; }
long srv_tls_recv(Client *c, void *buf, size_t len) { (void)c; (void)buf; (void)len; return -2; }
long srv_tls_send(Client *c, const void *buf, size_t len) { (void)c; (void)buf; (void)len; return -2; }
int srv_tls_pending(const Client *c) { (void)c; return 0; }

#endif
