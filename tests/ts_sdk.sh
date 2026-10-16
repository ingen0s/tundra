#!/usr/bin/env bash
# End-to-end tests for the TypeScript SDK: builds the server, starts it on
# spare ports (TCP plus WebSockets; a second one whose sessions expire at
# once; and, when built with TLS and openssl is available, a third one with
# built-in TLS on both ports), builds sdk/typescript and runs its node tests
# against them. Needs node 22+ and npm.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/sdk/typescript"
SERVER="$ROOT/build/tundra_chat_server"
# below the usual ephemeral range (32768+) so client sockets cannot collide
PORT="${TUNDRA_TEST_PORT:-$((20000 + RANDOM % 10000))}"
WSPORT=$((PORT + 1))
EXPPORT=$((PORT + 2))
TLSPORT=$((PORT + 3))
WSSPORT=$((PORT + 4))
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

(cd "$ROOT" && make -j8) >"$TMP/make.log" 2>&1 || { cat "$TMP/make.log"; exit 1; }

cd "$SDK" || exit 1
if [ -f package-lock.json ]; then npm ci --no-audit --no-fund; else npm install --no-audit --no-fund; fi >"$TMP/npm.log" 2>&1 \
    || { cat "$TMP/npm.log"; exit 1; }
npm run --silent build || exit 1

start_server "$TMP/main.log" -p "$PORT" -w "$WSPORT"
start_server "$TMP/expiry.log" -p "$EXPPORT" -g 0

# Built-in TLS: a CA (with the extensions OpenSSL 3 requires of a CA), a
# localhost certificate it signs, and an unrelated CA that must not be trusted.
if "$SERVER" -h 2>&1 | grep -q -- "-C  TLS" && command -v openssl >/dev/null; then
    if (
        cd "$TMP" &&
        openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj "/CN=Tundra test CA" \
            -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" &&
        openssl req -newkey rsa:2048 -nodes -keyout tls.key -out tls.csr -subj "/CN=localhost" &&
        printf '%s\n' 'subjectAltName=DNS:localhost,IP:127.0.0.1' 'basicConstraints=CA:FALSE' \
            'keyUsage=critical,digitalSignature,keyEncipherment' 'extendedKeyUsage=serverAuth' >san.ext &&
        openssl x509 -req -in tls.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out tls.pem -days 2 -extfile san.ext &&
        openssl req -x509 -newkey rsa:2048 -nodes -keyout other.key -out other-ca.pem -days 2 -subj "/CN=Other CA" \
            -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"
    ) >"$TMP/openssl.log" 2>&1; then
        start_server "$TMP/tls.log" -p "$TLSPORT" -w "$WSSPORT" -C "$TMP/tls.pem" -K "$TMP/tls.key"
        export TUNDRA_TLS_PORT="$TLSPORT" TUNDRA_WSS_PORT="$WSSPORT"
        export TUNDRA_TLS_CA="$TMP/ca.pem" TUNDRA_TLS_OTHER_CA="$TMP/other-ca.pem" NODE_EXTRA_CA_CERTS="$TMP/ca.pem"
    else
        cat "$TMP/openssl.log"
        exit 1
    fi
else
    echo "# built without TLS or no openssl command: TLS tests skipped"
fi

TUNDRA_PORT="$PORT" TUNDRA_WS_PORT="$WSPORT" TUNDRA_EXPIRY_PORT="$EXPPORT" \
    node --test --test-reporter=spec test/sdk.test.mjs
STATUS=$?

if [ "$STATUS" -ne 0 ]; then
    echo "--- server log ---"
    cat "$TMP/main.log"
fi
exit $STATUS
