/*
 * Optional SQLite persistence for chat messages.
 *
 * With -d PATH every message is written to the database as it is sent.
 * On startup the newest messages are loaded back into the in-memory log,
 * so message ids continue where they left off and catch-up keeps working
 * across restarts. HISTORY reads from the database, so it can page back
 * further than the in-memory log.
 */
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

static sqlite3 *db = NULL;
static sqlite3_stmt *insert_stmt = NULL;
static sqlite3_stmt *history_stmt = NULL;

int store_enabled(void) {
    return db != NULL;
}

static int exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        log_info("store: %s", err ? err : "error");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int prepare(const char *sql, sqlite3_stmt **stmt) {
    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) {
        log_info("store: %s", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static TundraBytes column_bytes(sqlite3_stmt *st, int col) {
    TundraBytes b;
    b.p = sqlite3_column_blob(st, col);
    b.len = (size_t)sqlite3_column_bytes(st, col);
    if (!b.p) b.len = 0;
    return b;
}

static int load_recent(void) {
    sqlite3_stmt *st;
    if (prepare("SELECT id, ts_ms, kind, target, sender, text FROM ("
                "  SELECT * FROM messages ORDER BY id DESC LIMIT ?1"
                ") ORDER BY id ASC", &st) < 0) {
        return -1;
    }
    sqlite3_bind_int(st, 1, MSGLOG_CAPACITY);
    size_t loaded = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        TundraBytes target = column_bytes(st, 3), text = column_bytes(st, 5);
        const char *sender = (const char *)sqlite3_column_text(st, 4);
        if (msglog_restore((uint64_t)sqlite3_column_int64(st, 0), (uint64_t)sqlite3_column_int64(st, 1),
                           (uint8_t)sqlite3_column_int(st, 2), target, sender ? sender : "", text) == 0) {
            loaded++;
        }
    }
    sqlite3_finalize(st);
    log_info("store: loaded %zu recent message(s), next id %llu", loaded,
             (unsigned long long)(msglog_last_id() + 1));
    return 0;
}

int store_open(const char *path) {
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        log_info("store: cannot open %s: %s", path, db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        db = NULL;
        return -1;
    }
    if (exec("PRAGMA journal_mode=WAL;"
             "PRAGMA synchronous=NORMAL;"
             "CREATE TABLE IF NOT EXISTS messages ("
             "  id INTEGER PRIMARY KEY,"
             "  ts_ms INTEGER NOT NULL,"
             "  kind INTEGER NOT NULL,"
             "  target BLOB NOT NULL,"
             "  sender TEXT NOT NULL,"
             "  text BLOB NOT NULL);"
             "CREATE INDEX IF NOT EXISTS messages_by_target ON messages(kind, target, id);"
             "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);") < 0 ||
        prepare("INSERT INTO messages (id, ts_ms, kind, target, sender, text) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                &insert_stmt) < 0 ||
        prepare("SELECT id, ts_ms, kind, target, sender, text FROM messages"
                " WHERE kind = 0 AND target = ?1 AND id < ?2 ORDER BY id DESC LIMIT ?3",
                &history_stmt) < 0 ||
        load_recent() < 0) {
        store_close();
        return -1;
    }
    return 0;
}

void store_close(void) {
    sqlite3_finalize(insert_stmt);
    sqlite3_finalize(history_stmt);
    insert_stmt = history_stmt = NULL;
    if (db) sqlite3_close(db);
    db = NULL;
}

void store_append(const LoggedMessage *m) {
    if (!db) return;
    sqlite3_stmt *st = insert_stmt;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)m->id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)m->ts_ms);
    sqlite3_bind_int(st, 3, m->kind);
    sqlite3_bind_blob(st, 4, m->target, (int)strlen(m->target), SQLITE_STATIC);
    sqlite3_bind_text(st, 5, m->sender, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 6, m->text_len ? (const void *)m->text : "", (int)m->text_len, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) log_info("store: insert failed: %s", sqlite3_errmsg(db));
    sqlite3_reset(st);
}

void store_room_deleted(const char *room, uint64_t last_id) {
    if (!db) return;
    sqlite3_stmt *st;
    if (prepare("INSERT OR REPLACE INTO meta (key, value) VALUES ('deleted:' || ?1, ?2)", &st) < 0) return;
    sqlite3_bind_text(st, 1, room, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)last_id);
    if (sqlite3_step(st) != SQLITE_DONE) log_info("store: cannot record deletion of %s: %s", room, sqlite3_errmsg(db));
    sqlite3_finalize(st);
}

uint64_t store_room_floor(const char *room) {
    if (!db) return 0;
    sqlite3_stmt *st;
    uint64_t floor = 0;
    if (prepare("SELECT value FROM meta WHERE key = 'deleted:' || ?1", &st) < 0) return 0;
    sqlite3_bind_text(st, 1, room, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) floor = (uint64_t)sqlite3_column_int64(st, 0) + 1;
    sqlite3_finalize(st);
    return floor;
}

size_t store_history(const char *room, uint64_t before_id, size_t limit, LoggedMessage *out) {
    if (!db) return 0;
    sqlite3_stmt *st = history_stmt;
    sqlite3_reset(st);
    sqlite3_bind_blob(st, 1, room, (int)strlen(room), SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, before_id ? (sqlite3_int64)before_id : INT64_MAX);
    sqlite3_bind_int(st, 3, (int)limit);
    size_t n = 0;
    while (n < limit && sqlite3_step(st) == SQLITE_ROW) {
        LoggedMessage *m = &out[n];
        memset(m, 0, sizeof(*m));
        TundraBytes target = column_bytes(st, 3), text = column_bytes(st, 5);
        const char *sender = (const char *)sqlite3_column_text(st, 4);
        m->id = (uint64_t)sqlite3_column_int64(st, 0);
        m->ts_ms = (uint64_t)sqlite3_column_int64(st, 1);
        m->kind = (uint8_t)sqlite3_column_int(st, 2);
        size_t tl = target.len < sizeof(m->target) - 1 ? target.len : sizeof(m->target) - 1;
        if (tl) memcpy(m->target, target.p, tl);
        if (sender) strncpy(m->sender, sender, sizeof(m->sender) - 1);
        m->text = malloc(text.len ? text.len : 1);
        if (!m->text) break;
        if (text.len) memcpy(m->text, text.p, text.len);
        m->text_len = text.len;
        n++;
    }
    sqlite3_reset(st);
    return n; /* newest first */
}
