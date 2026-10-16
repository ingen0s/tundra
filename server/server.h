/* Shared state and interfaces for the Tundra server modules. */
#ifndef TUNDRA_SERVER_H
#define TUNDRA_SERVER_H

#include <stdint.h>
#include <time.h>
#include "../tundra_platform.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#include "../tundra_codec.h"

#define DEFAULT_PORT          4242
#define MAX_CLIENTS           1000
#define MAX_SESSIONS          MAX_CLIENTS
#define MAX_ROOMS             256
#define LOBBY                 "#lobby"
#define DEFAULT_IDLE_TIMEOUT  120       /* seconds without any frame before a connection is dropped */
#define DEFAULT_RESUME_GRACE  120       /* seconds a disconnected session is held for reconnect */
#define REGISTER_TIMEOUT      10        /* seconds a new connection has to send REGISTER */
#define PING_INTERVAL         30        /* seconds */
#define MAX_BUFFERED_MESSAGES 50        /* non-chat frames per session backlog; oldest dropped first */
#define MAX_WRITE_MARKS       256       /* chat ids tracked per connection until written */
#define MAX_BACKLOG_BYTES     (1u << 20)   /* per session backlog */
#define MAX_TOTAL_BACKLOG     (64u << 20)  /* all backlogs together */
#define MAX_OUTBOUND_BYTES    (1u << 20)   /* per client; slower clients are disconnected */
#define REPLAY_HIGH_WATER     (256u << 10) /* backlog replay pauses while this much is queued */
#define MAX_READS_PER_EVENT   16           /* fairness: other clients get a turn */
#define MAX_PENDING_REQUESTS  4096         /* open agent requests, server-wide */
#define MAX_PENDING_PER_SESSION 64         /* open agent requests per requester */
#define MAX_PENDING_PER_AGENT 256          /* open agent requests per agent */
#define DEFAULT_AGENT_TIMEOUT 60           /* seconds an agent request may stay open */
#define MSGLOG_CAPACITY       10000     /* recent messages kept in memory for history */
#define MSGLOG_MAX_BYTES      (64u << 20) /* and at most this much message text */

typedef enum { SESSION_FREE = 0, SESSION_ACTIVE, SESSION_DETACHED } SessionState;

typedef struct {
    uint8_t type;
    uint8_t *payload;
    size_t len;
    uint64_t after_id;          /* last message id when buffered: sent once catch-up passes it */
} BufferedFrame;

struct Client;

typedef struct Session {
    SessionState state;
    tundra_uuid_t id;
    char username[TUNDRA_MAX_USERNAME + 1];
    struct Client *client;      /* set only while ACTIVE */
    time_t connected_at;
    uint64_t detached_at_ms;    /* monotonic; ms so eviction order has no ties */
    uint16_t out_seq;           /* sequence counter for server-initiated frames */
    /* Frames waiting for this session, oldest first. Filled while the
     * session is detached, and while an earlier backlog is still being
     * replayed (so live frames never overtake buffered ones). */
    BufferedFrame backlog[MAX_BUFFERED_MESSAGES];
    size_t backlog_head;        /* index of oldest entry */
    size_t backlog_count;
    size_t backlog_bytes;
    uint32_t backlog_dropped;   /* frames dropped since the last resume */
    int pumping;                /* session_pump is on the stack */
    /* Chat catch-up. CHAT frames are never buffered: the message log has
     * them, and a session that is behind replays from it. */
    uint64_t replay_next;       /* next log id to consider; 0 = not replaying */
    uint64_t start_id;          /* last message id when the session was created: never replay before it */
    uint64_t written_id;        /* highest CHAT id fully written to a socket */
    uint64_t acked_id;          /* highest CHAT id the client acknowledged */
    int has_acked;
    uint32_t latency_ms;
    uint32_t messages_sent;
    uint32_t messages_received;
    uint8_t auth_token[256];    /* token presented at registration, for re-checks */
    size_t auth_token_len;
    int is_agent;
    uint8_t *capabilities;      /* as advertised; opaque to the server */
    size_t capabilities_len;
} Session;

enum { PROTO_TCP = 0, PROTO_HTTP, PROTO_WS };

typedef struct Client {
    int fd;                     /* slot number for logs; -1 when the slot is free */
    tsock_t sock;
    int proto;                  /* PROTO_*: raw Tundra, HTTP before upgrade, WebSocket */
    int dead;                   /* closed at the end of the current loop iteration */
    int close_after_flush;
    int want_write;             /* EPOLLOUT currently registered */
    Session *session;
    time_t accepted_at;
    time_t last_seen;
    uint8_t *out;
    size_t out_len;
    size_t out_cap;
    /* where each queued CHAT frame ends in `out`, oldest first */
    struct { size_t end; uint64_t id; } marks[MAX_WRITE_MARKS];
    size_t marks_head;
    size_t marks_count;
    size_t in_len;
    uint8_t in[TUNDRA_MAX_FRAME + 16]; /* one frame plus a WebSocket header; keep last: reset code clears everything before it */
} Client;

typedef struct {
    int used;
    char name[TUNDRA_MAX_ROOM + 1];
    uint64_t first_msg_id;      /* messages before this belong to an earlier room of the same name */
    uint8_t members[(MAX_SESSIONS + 7) / 8]; /* bit per session index */
    uint64_t joined_after[MAX_SESSIONS];     /* last message id when each member joined */
    int member_count;
} Room;

extern Client clients[MAX_CLIENTS];
extern Session sessions[MAX_SESSIONS];
extern int verbose;
extern int idle_timeout;
extern int resume_grace;

/* ---- util.c ---- */
#if defined(__MINGW32__)
#define TUNDRA_PRINTF_FMT gnu_printf /* C99 stdio via __USE_MINGW_ANSI_STDIO */
#else
#define TUNDRA_PRINTF_FMT printf
#endif
void log_info(const char *fmt, ...) __attribute__((format(TUNDRA_PRINTF_FMT, 1, 2)));
void log_debug(const char *fmt, ...) __attribute__((format(TUNDRA_PRINTF_FMT, 1, 2)));
uint64_t wall_ms(void);
uint64_t mono_ms(void);
time_t mono_s(void);
int valid_username(const uint8_t *name, size_t len);
int valid_room_name(const uint8_t *name, size_t len);

/* ---- net.c ---- */
int net_listen(int port);
int net_listen_ws(int port);               /* optional HTTP/WebSocket listener */
void out_append(Client *c, const uint8_t *data, size_t len); /* raw bytes, then flush */
void net_run(void (*tick)(void));                          /* event loop; returns on shutdown */
/* Ctrl+C / SIGINT / SIGTERM stop net_run; SIGHUP calls on_reload (POSIX). */
void net_handle_signals(void (*on_reload)(void));
void net_shutdown(void);
void mark_dead(Client *c);
void close_after_flush(Client *c);
/* Reply to a request: echoes the request's sequence number. */
void send_frame(Client *c, uint8_t type, uint16_t seq, const uint8_t *payload, size_t len);
/* Server-initiated frame: uses the session's own sequence counter. */
void push_frame(Client *c, uint8_t type, const uint8_t *payload, size_t len);
/* Encodes `msg` and sends it as a reply (seq) or push (when push != 0). */
void send_msg(Client *c, const TundraMsg *msg, uint16_t seq, int push);
void send_error(Client *c, uint16_t seq, uint16_t code, const char *text);
size_t build_error(uint8_t *buf, size_t cap, uint16_t code, const char *text);
void send_system(Client *c, uint16_t seq, const tundra_uuid_t id, const char *text);

/* ---- session.c ---- */
int session_index(const Session *s);
Session *find_session_by_id(const tundra_uuid_t id);
Session *find_session_by_name(const char *name);
Session *find_session_by_bytes(TundraBytes name);
Session *alloc_session(void); /* may release the longest-detached session */
void release_session(Session *s);
void attach(Session *s, Client *c);
void detach(Client *c);
/* Delivers a frame to a session. Sent now if the session is online and
 * nothing is pending ahead of it. Otherwise non-chat frames are buffered,
 * and chat frames are left to the replay (the message log has them). */
void deliver(Session *s, uint8_t type, const uint8_t *payload, size_t len);
/* Sends buffered frames, then any pending chat replay, while the
 * connection has room; the rest follow from flush_client as it drains. */
void session_pump(Session *s);
int session_has_pending(const Session *s);
/* Starts catching a resumed session up on chats after `after_id`.
 * Returns 1 if older messages it may have needed were already evicted. */
int chat_replay_start(Session *s, uint64_t after_id);
void backlog_clear(Session *s);
void sessions_expire(void);
void sessions_ping(void);
/* After the tokens file changes: ends sessions whose name no longer
 * accepts the token they registered with. */
void sessions_recheck_auth(void);

/* ---- rooms.c ---- */
Room *room_find(TundraBytes name);
Room *room_find_cstr(const char *name);
Room *room_get_or_create(TundraBytes name);
int room_is_member(const Room *r, const Session *s);
uint64_t room_joined_after(const Room *r, const Session *s);
void room_add(Room *r, Session *s);
void room_remove(Room *r, Session *s);   /* deletes the room when it empties (except the lobby) */
void rooms_remove_session(Session *s);   /* on expiry: leaves every room, with events */
/* Delivers a payload to every member except `skip` (may be NULL). */
void room_deliver(Room *r, const Session *skip, uint8_t type, const uint8_t *payload, size_t len);
void room_event(Room *r, uint8_t event, const Session *who); /* to members and `who` */
Room *rooms_iter(int *cursor);

/* ---- msglog.c ---- */
typedef struct {
    uint64_t id;
    uint64_t ts_ms;
    uint8_t kind;
    char target[TUNDRA_MAX_USERNAME > TUNDRA_MAX_ROOM ? TUNDRA_MAX_USERNAME + 1 : TUNDRA_MAX_ROOM + 1];
    char sender[TUNDRA_MAX_USERNAME + 1];
    uint8_t *text;
    size_t text_len;
} LoggedMessage;

/* Appends a message and returns its stored copy (with id assigned), or NULL. */
const LoggedMessage *msglog_append(uint8_t kind, TundraBytes target, const char *sender, TundraBytes text);
const LoggedMessage *msglog_get(uint64_t id);   /* NULL if never assigned or evicted */
uint64_t msglog_last_id(void);
uint64_t msglog_first_id(void);  /* oldest id still held (last_id + 1 when empty) */
void msglog_to_chat(const LoggedMessage *m, TundraMsg *out);
/* Re-inserts a stored message at startup; ids must be increasing. 0 on success. */
int msglog_restore(uint64_t id, uint64_t ts_ms, uint8_t kind, TundraBytes target, const char *sender, TundraBytes text);
void msglog_free(void);

/* ---- auth.c ---- */
extern int auth_require_all;
int auth_init(const char *path);   /* 0 on success */
void auth_reload(void);
void auth_free(void);
/* 1 if `username` may register with `token` (NULL when none was sent). */
int auth_check(const char *username, const TundraBytes *token);

/* ---- store.c ---- */
int store_open(const char *path);  /* 0 on success; loads recent messages */
void store_close(void);
int store_enabled(void);
void store_append(const LoggedMessage *m);
/* Room deletions are remembered so a recreated room starts with an empty
 * history, across restarts too. */
void store_room_deleted(const char *room, uint64_t last_id);
uint64_t store_room_floor(const char *room); /* first message id the room may show */
/* Ids are reserved in the database in blocks before they are used, so a
 * failed insert can never lead to an id being reused after a restart. */
int store_reserve(uint64_t id);    /* 0 on success (always 0 without a store) */
uint64_t store_next_id(void);      /* first id to use after loading, 0 without a store */
/* Room history before `before_id` (0 = latest), newest first. Fills `out`
 * with up to `limit` messages whose text the caller must free. */
size_t store_history(const char *room, uint64_t before_id, size_t limit, LoggedMessage *out);

/* ---- agents.c ---- */
extern int agent_timeout;
void handle_agent_advertise(Client *c, uint16_t seq, TundraBytes caps);
void handle_agent_request(Client *c, uint16_t seq, const TundraMsg *m);
void handle_agent_response(Client *c, uint16_t seq, const TundraMsg *m);
void handle_list_agents(Client *c, uint16_t seq);
void agents_expire(void);
/* Closes requests to and from `s`, and forgets its capabilities. */
void agents_session_released(Session *s);
/* An AGENT_REQUEST_IN frame was dropped from a backlog: closes its request. */
void agents_request_dropped(const uint8_t *payload, size_t len);

/* ---- ws.c ---- */
void ws_process_input(Client *c);          /* HTTP request or WebSocket frames */
size_t ws_header(uint8_t *hdr, size_t payload_len);
void tundra_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

/* ---- handlers.c ---- */
void handle_frame(Client *c, const uint8_t *frame, size_t payload_len);

#endif /* TUNDRA_SERVER_H */
