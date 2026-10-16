#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "server.h"

static time_t last_ping;
static volatile int reload_requested = 0;

static void on_hup(void) {
    reload_requested = 1;
}

static void tick(void) {
    if (reload_requested) {
        reload_requested = 0;
        auth_reload();
    }
    sessions_expire();
    agents_expire();
    time_t now = mono_s();
    if (now - last_ping >= PING_INTERVAL) {
        sessions_ping();
        last_ping = now;
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p port] [-w http-port] [-d database] [-t tokens-file [-A]] [-i idle-timeout-s] [-g resume-grace-s]\n"
            "          [-T agent-timeout-s] [-v]\n"
            "  -w  also serve the web client and WebSockets (GET /ws) on this port\n"
            "  -d  keep messages in this SQLite database (history survives restarts)\n"
            "  -t  \"username token\" file: listed names must present their token (re-read on SIGHUP)\n"
            "  -A  with -t, every username must be listed\n",
            prog);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    const char *db_path = NULL;
    const char *tokens_path = NULL;
    int ws_port = 0;
    int opt;
    while ((opt = getopt(argc, argv, "p:w:d:t:Ai:g:T:vh")) != -1) {
        switch (opt) {
            case 'd': db_path = optarg; break;
            case 'w': ws_port = atoi(optarg); break;
            case 't': tokens_path = optarg; break;
            case 'A': auth_require_all = 1; break;
            case 'p': port = atoi(optarg); break;
            case 'i': idle_timeout = atoi(optarg); break;
            case 'g': resume_grace = atoi(optarg); break;
            case 'T': agent_timeout = atoi(optarg); break;
            case 'v': verbose = 1; break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (port <= 0 || port > 65535 || ws_port < 0 || ws_port > 65535 || idle_timeout <= 0 || resume_grace < 0 || agent_timeout <= 0) {
        usage(argv[0]);
        return 1;
    }

    if (auth_require_all && !tokens_path) {
        fprintf(stderr, "-A needs -t\n");
        return 1;
    }
    /* before anything slow, so an early SIGHUP cannot kill the process */
    net_handle_signals(on_hup);
    if (tokens_path && auth_init(tokens_path) < 0) return 1;
    if (db_path && store_open(db_path) < 0) return 1;

    if (net_listen(port) < 0) return 1;
    if (ws_port && net_listen_ws(ws_port) < 0) return 1;
    if (ws_port) log_info("web client and WebSockets on http://localhost:%d/", ws_port);
    log_info("Tundra server listening on port %d", port);

    last_ping = mono_s();
    net_run(tick);

    net_shutdown();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        backlog_clear(&sessions[i]);
        free(sessions[i].capabilities);
    }
    msglog_free();
    store_close();
    auth_free();
    return 0;
}
