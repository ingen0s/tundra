#define _GNU_SOURCE
/*
 * Optional token authentication.
 *
 * The tokens file has one "username token" pair per line; blank lines and
 * lines starting with '#' are ignored. A username listed in the file can
 * only register with its token (REGISTER extension REG_TOKEN). Names not
 * in the file are open, unless the server runs with -A, which requires
 * every username to be listed. The file is re-read on SIGHUP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

#define MAX_AUTH_ENTRIES 4096
#define MAX_TOKEN_LEN    256

typedef struct {
    char username[TUNDRA_MAX_USERNAME + 1];
    uint8_t token[MAX_TOKEN_LEN];
    size_t token_len;
} AuthEntry;

static AuthEntry *entries = NULL;
static size_t entry_count = 0;
static char *auth_path = NULL;
int auth_require_all = 0;

int auth_enabled(void) {
    return auth_path != NULL;
}

/* Parses the file into a fresh table; the old table is kept on error. */
static int load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        log_info("auth: cannot open %s", path);
        return -1;
    }
    AuthEntry *table = calloc(MAX_AUTH_ENTRIES, sizeof(AuthEntry));
    if (!table) {
        fclose(f);
        return -1;
    }
    size_t count = 0;
    char line[1024];
    int lineno = 0, bad = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char user[64], token[MAX_TOKEN_LEN + 1];
        if (sscanf(p, "%63s %256s", user, token) != 2 ||
            !valid_username((const uint8_t *)user, strlen(user)) || strlen(token) > MAX_TOKEN_LEN) {
            log_info("auth: %s:%d: expected \"username token\"", path, lineno);
            bad = 1;
            continue;
        }
        if (count == MAX_AUTH_ENTRIES) {
            log_info("auth: more than %d entries, ignoring the rest", MAX_AUTH_ENTRIES);
            break;
        }
        AuthEntry *e = &table[count++];
        strcpy(e->username, user);
        e->token_len = strlen(token);
        memcpy(e->token, token, e->token_len);
    }
    fclose(f);
    if (bad && count == 0) {
        free(table);
        return -1;
    }
    free(entries);
    entries = table;
    entry_count = count;
    log_info("auth: %zu user(s) loaded from %s", count, path);
    return 0;
}

int auth_init(const char *path) {
    free(auth_path);
    auth_path = strdup(path);
    return auth_path ? load(auth_path) : -1;
}

void auth_reload(void) {
    if (auth_path) load(auth_path);
}

void auth_free(void) {
    free(entries);
    entries = NULL;
    entry_count = 0;
    free(auth_path);
    auth_path = NULL;
}

/* Compares in time that depends only on the stored token's length. */
static int token_equal(const AuthEntry *e, TundraBytes given) {
    uint8_t diff = (uint8_t)(given.len != e->token_len);
    for (size_t i = 0; i < e->token_len; i++) {
        uint8_t g = i < given.len ? given.p[i] : 0;
        diff |= (uint8_t)(g ^ e->token[i]);
    }
    return diff == 0;
}

int auth_check(const char *username, const TundraBytes *token) {
    if (!auth_path) return 1;
    for (size_t i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].username, username) == 0) return token && token_equal(&entries[i], *token);
    }
    return !auth_require_all;
}
