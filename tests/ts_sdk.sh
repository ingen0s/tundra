#!/usr/bin/env bash
# End-to-end tests for the TypeScript SDK: builds the server, starts it on
# spare ports (TCP plus WebSockets, and a second one whose sessions expire at
# once), builds sdk/typescript and runs its node tests against them.
# Needs node 22+ and npm.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/sdk/typescript"
SERVER="$ROOT/build/tundra_chat_server"
# below the usual ephemeral range (32768+) so client sockets cannot collide
PORT="${TUNDRA_TEST_PORT:-$((20000 + RANDOM % 10000))}"
WSPORT=$((PORT + 1))
EXPPORT=$((PORT + 2))
TMP="$(mktemp -d)"
PIDS=()

cleanup() {
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# start_server LOG [server args...]
start_server() {
    local log="$1"
    shift
    "$SERVER" "$@" 2>"$log" &
    PIDS+=($!)
    for _ in $(seq 100); do
        grep -q "listening on port" "$log" 2>/dev/null && return 0
        sleep 0.05
    done
    echo "server did not start:" >&2
    cat "$log" >&2
    exit 1
}

(cd "$ROOT" && make) >"$TMP/make.log" 2>&1 || { cat "$TMP/make.log"; exit 1; }

cd "$SDK" || exit 1
if [ -f package-lock.json ]; then npm ci --no-audit --no-fund; else npm install --no-audit --no-fund; fi >"$TMP/npm.log" 2>&1 \
    || { cat "$TMP/npm.log"; exit 1; }
npm run --silent build || exit 1

start_server "$TMP/main.log" -p "$PORT" -w "$WSPORT"
start_server "$TMP/expiry.log" -p "$EXPPORT" -g 0

TUNDRA_PORT="$PORT" TUNDRA_WS_PORT="$WSPORT" TUNDRA_EXPIRY_PORT="$EXPPORT" \
    node --test --test-reporter=spec test/sdk.test.mjs
STATUS=$?

if [ "$STATUS" -ne 0 ]; then
    echo "--- server log ---"
    cat "$TMP/main.log"
fi
exit $STATUS
