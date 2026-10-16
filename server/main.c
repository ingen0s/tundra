#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "server.h"

static time_t last_ping;

static void tick(void) {
    sessions_expire();
    time_t now = mono_s();
    if (now - last_ping >= PING_INTERVAL) {
        sessions_ping();
        last_ping = now;
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-p port] [-v]\n", prog);
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    int opt;
    while ((opt = getopt(argc, argv, "p:vh")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'v': verbose = 1; break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (port <= 0 || port > 65535) {
        usage(argv[0]);
        return 1;
    }

    if (net_listen(port) < 0) return 1;
    log_info("Tundra server listening on port %d", port);

    last_ping = mono_s();
    net_run(tick);

    net_shutdown();
    for (int i = 0; i < MAX_SESSIONS; i++) backlog_clear(&sessions[i]);
    msglog_free();
    return 0;
}
