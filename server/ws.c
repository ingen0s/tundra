/*
 * WebSocket gateway (RFC 6455) and a tiny HTTP responder.
 *
 * With -w PORT the server also listens for HTTP. GET / returns the bundled
 * web client; GET /ws upgrades to a WebSocket. Each binary WebSocket
 * message then carries exactly one Tundra frame in each direction, so
 * browser clients speak the same protocol as TCP clients.
 *
 * Kept deliberately small: client data messages must be unfragmented
 * (which is what browsers send), text messages are refused, and messages
 * larger than one Tundra frame close the connection.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "server.h"
#include "web_index.h" /* generated from web/index.html: web_index_html[], web_index_html_len */

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define MAX_HTTP_REQUEST 8192

/* ---- SHA-1 (only for the handshake) ---- */

static uint32_t rol(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void tundra_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t rest = len - i;
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    size_t tail_len = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) tail[tail_len - 1 - j] = (uint8_t)(bits >> (8 * j));
    sha1_block(h, tail);
    if (tail_len == 128) sha1_block(h, tail + 64);
    for (int j = 0; j < 5; j++) {
        out[4 * j] = (uint8_t)(h[j] >> 24);
        out[4 * j + 1] = (uint8_t)(h[j] >> 16);
        out[4 * j + 2] = (uint8_t)(h[j] >> 8);
        out[4 * j + 3] = (uint8_t)h[j];
    }
}

static size_t base64(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* ---- HTTP ---- */

/* Finds a header value (case-insensitive name) in a request; copies it,
 * trimmed, into `out`. */
static int header_value(const char *req, const char *name, char *out, size_t cap) {
    size_t nlen = strlen(name);
    const char *line = strstr(req, "\r\n");
    while (line && line[2] != '\r') {
        line += 2;
        if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
            const char *v = line + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *end = strstr(v, "\r\n");
            if (!end) return 0;
            while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
            size_t n = (size_t)(end - v);
            if (n >= cap) return 0;
            memcpy(out, v, n);
            out[n] = '\0';
            return 1;
        }
        line = strstr(line, "\r\n");
    }
    return 0;
}

static int contains_token(const char *value, const char *token) {
    /* comma-separated, case-insensitive */
    size_t tlen = strlen(token);
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && end[-1] == ' ') end--;
        if ((size_t)(end - start) == tlen && strncasecmp(start, token, tlen) == 0) return 1;
    }
    return 0;
}

static void http_reply(Client *c, const char *status, const char *type, const uint8_t *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, len);
    out_append(c, (const uint8_t *)head, (size_t)n);
    if (body && len) out_append(c, body, len);
    close_after_flush(c);
}

/* Handles a buffered HTTP request. Returns the bytes consumed, or 0 if the
 * request is not complete yet. */
static size_t handle_http(Client *c) {
    /* Only the headers count toward the limit: a client may send WebSocket
     * frames right behind the upgrade request in the same write. */
    size_t head = c->in_len < MAX_HTTP_REQUEST ? c->in_len : MAX_HTTP_REQUEST;
    /* the buffer is not NUL-terminated; work on a terminated copy */
    char req[MAX_HTTP_REQUEST + 1];
    memcpy(req, c->in, head);
    req[head] = '\0';
    char *end = strstr(req, "\r\n\r\n");
    if (!end) {
        if (c->in_len < MAX_HTTP_REQUEST) return 0; /* wait for the rest */
        http_reply(c, "431 Request Header Fields Too Large", "text/plain", (const uint8_t *)"too large\n", 10);
        return c->in_len;
    }
    size_t consumed = (size_t)(end - req) + 4;
    end[2] = '\0'; /* keep the last header's CRLF for header_value */

    char method[8], path[256];
    if (sscanf(req, "%7s %255s", method, path) != 2 || strcmp(method, "GET") != 0) {
        http_reply(c, "405 Method Not Allowed", "text/plain", (const uint8_t *)"GET only\n", 9);
        return consumed;
    }

    char upgrade[64], connection[128], key[64], version[8];
    int wants_ws = header_value(req, "Upgrade", upgrade, sizeof(upgrade)) && strcasecmp(upgrade, "websocket") == 0;
    if (strcmp(path, "/ws") == 0 && wants_ws) {
        if (!header_value(req, "Connection", connection, sizeof(connection)) || !contains_token(connection, "upgrade") ||
            !header_value(req, "Sec-WebSocket-Key", key, sizeof(key)) || strlen(key) != 24 || /* base64 of 16 bytes */
            !header_value(req, "Sec-WebSocket-Version", version, sizeof(version)) || strcmp(version, "13") != 0) {
            http_reply(c, "400 Bad Request", "text/plain", (const uint8_t *)"bad websocket handshake\n", 24);
            return consumed;
        }
        char concat[128];
        int n = snprintf(concat, sizeof(concat), "%s%s", key, WS_GUID);
        uint8_t digest[20];
        tundra_sha1((const uint8_t *)concat, (size_t)n, digest);
        char accept[32];
        base64(digest, sizeof(digest), accept);
        char resp[256];
        n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
        c->proto = PROTO_WS;
        out_append(c, (const uint8_t *)resp, (size_t)n);
        log_info("fd %d upgraded to websocket", c->fd);
        return consumed;
    }
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        http_reply(c, "200 OK", "text/html; charset=utf-8", web_index_html, web_index_html_len);
        return consumed;
    }
    http_reply(c, "404 Not Found", "text/plain", (const uint8_t *)"not found\n", 10);
    return consumed;
}

/* ---- WebSocket frames ---- */

size_t ws_header(uint8_t *hdr, size_t payload_len) {
    hdr[0] = 0x82; /* FIN + binary */
    if (payload_len < 126) {
        hdr[1] = (uint8_t)payload_len;
        return 2;
    }
    if (payload_len <= 0xFFFF) {
        hdr[1] = 126;
        tundra_put_u16(hdr + 2, (uint16_t)payload_len);
        return 4;
    }
    hdr[1] = 127;
    tundra_put_u64(hdr + 2, payload_len);
    return 10;
}

static void ws_control(Client *c, uint8_t opcode, const uint8_t *payload, size_t len) {
    uint8_t hdr[2] = { (uint8_t)(0x80 | opcode), (uint8_t)len }; /* control payloads are < 126 */
    out_append(c, hdr, 2);
    if (len) out_append(c, payload, len);
}

static void ws_close(Client *c, uint16_t code) {
    uint8_t body[2];
    tundra_put_u16(body, code);
    ws_control(c, 0x8, body, 2);
    close_after_flush(c);
}

/* Parses complete WebSocket frames from the input buffer. Returns the
 * bytes consumed. */
static size_t handle_ws(Client *c) {
    size_t off = 0;
    while (!c->dead && !c->close_after_flush) {
        uint8_t *p = c->in + off;
        size_t avail = c->in_len - off;
        if (avail < 2) break;
        int fin = p[0] & 0x80;
        uint8_t opcode = p[0] & 0x0F;
        int masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = tundra_get_u16(p + 2);
            hdr = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = tundra_get_u64(p + 2);
            hdr = 10;
        }
        if (!masked || (p[0] & 0x70)) { /* clients must mask; no extensions negotiated */
            ws_close(c, 1002);
            break;
        }
        if (opcode >= 0x8 && (!fin || len > 125)) { /* control frames: unfragmented, short */
            ws_close(c, 1002);
            break;
        }
        if (len > TUNDRA_MAX_FRAME) {
            ws_close(c, 1009);
            break;
        }
        if (avail < hdr + 4 + len) break;
        const uint8_t *mask = p + hdr;
        uint8_t *data = p + hdr + 4;
        for (uint64_t i = 0; i < len; i++) data[i] ^= mask[i % 4];
        off += hdr + 4 + (size_t)len;

        switch (opcode) {
            case 0x2: /* binary: exactly one Tundra frame */
                if (!fin || len < TUNDRA_HEADER_LEN || (uint64_t)TUNDRA_HEADER_LEN + tundra_get_u16(data + 4) != len) {
                    ws_close(c, 1007);
                    return off;
                }
                handle_frame(c, data, (size_t)len - TUNDRA_HEADER_LEN);
                break;
            case 0x8: /* close: echo the client's code, or 1000 when it sent none */
                if (len == 1) {
                    ws_close(c, 1002);
                } else {
                    ws_close(c, len >= 2 ? tundra_get_u16(data) : 1000);
                }
                return off;
            case 0x9: /* ping */
                ws_control(c, 0xA, data, (size_t)len);
                break;
            case 0xA: /* pong */
                break;
            default: /* text, continuation, reserved */
                ws_close(c, 1003);
                return off;
        }
    }
    return off;
}

static void consume(Client *c, size_t used) {
    if (used == 0) return;
    if (used >= c->in_len) {
        c->in_len = 0;
        return;
    }
    memmove(c->in, c->in + used, c->in_len - used);
    c->in_len -= used;
}

void ws_process_input(Client *c) {
    if (c->proto == PROTO_HTTP) {
        consume(c, handle_http(c));
        if (c->proto != PROTO_WS) return;
        /* bytes after the handshake are WebSocket frames */
    }
    consume(c, handle_ws(c));
}
