#!/usr/bin/env bash
# End-to-end tests: starts a server on a spare port and drives it with the
# test client. Run via `make test` (or `make asan && ./tests/integration.sh`).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build/tundra_chat_server"
CLIENT="$ROOT/build/tundra_client"
PORT="${TUNDRA_TEST_PORT:-47$((RANDOM % 900 + 100))}"
TMP="$(mktemp -d)"
FAILED=0

cleanup() {
    [ -n "${SERVER_PID:-}" ] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

client() { "$CLIENT" -p "$PORT" "$@"; }

pass() { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; FAILED=1; }

expect() { # name file pattern
    if grep -qF -- "$3" "$2"; then pass "$1"; else fail "$1 (missing: $3)"; sed 's/^/       /' "$2"; fi
}

"$SERVER" -p "$PORT" 2>"$TMP/server.log" &
SERVER_PID=$!
for _ in $(seq 50); do
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && break
    sleep 0.1
done

# 1. broadcast between two users, join notice
client -u bob -w 2 >"$TMP/bob.out" &
BOB=$!
sleep 0.3
client -u alice -m "hello bob" -w 0.3 >"$TMP/alice.out"
wait $BOB
expect "register returns a session id" "$TMP/alice.out" "SESSION "
expect "welcome message" "$TMP/alice.out" "SYSTEM Welcome, alice!"
expect "join notice reaches other users" "$TMP/bob.out" "SYSTEM alice has joined the chat."
expect "chat message is broadcast" "$TMP/bob.out" "MSG alice hello bob"
if grep -qF "MSG alice" "$TMP/alice.out"; then fail "sender does not receive own message"; else pass "sender does not receive own message"; fi

# 2. ordering of several messages
client -u erin -w 1.5 >"$TMP/erin.out" &
ERIN=$!
sleep 0.3
client -u frank -m one -m two -m three -w 0.3 >/dev/null
wait $ERIN
if grep '^MSG frank' "$TMP/erin.out" | awk '{print $3}' | tr '\n' ' ' | grep -qx 'one two three '; then
    pass "messages arrive in order"
else
    fail "messages arrive in order"; sed 's/^/       /' "$TMP/erin.out"
fi

# 3. duplicate username is rejected while the first user is connected
client -u gina -w 1.5 >/dev/null &
GINA=$!
sleep 0.3
client -u gina -w 0.3 >"$TMP/dup.out"
wait $GINA
expect "duplicate username rejected" "$TMP/dup.out" "ERROR 1 Username already taken"

# 4. invalid username
client -u "has space" -w 0.3 >"$TMP/bad.out"
expect "invalid username rejected" "$TMP/bad.out" "ERROR 2 "

# 5. disconnect, miss messages, reconnect with session id, get the backlog
client -u carol -w 0 >"$TMP/carol1.out"
CAROL_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/carol1.out")"
sleep 0.2
client -u dave -m "while you were out" -m "second one" -w 0.3 >/dev/null
client -u carol -r "$CAROL_ID" -w 0.5 >"$TMP/carol2.out"
expect "resume is recognised" "$TMP/carol2.out" "SYSTEM Welcome back, carol!"
expect "resume keeps the same session id" "$TMP/carol2.out" "SESSION $CAROL_ID"
expect "buffered message 1 delivered on resume" "$TMP/carol2.out" "MSG dave while you were out"
expect "buffered message 2 delivered on resume" "$TMP/carol2.out" "MSG dave second one"

# 6. a session id held by someone else cannot be used to take a different name
client -u mallory -r "$CAROL_ID" -w 0.3 >"$TMP/mallory.out"
if grep -qF "SESSION $CAROL_ID" "$TMP/mallory.out"; then
    fail "session id is bound to its username"
else
    pass "session id is bound to its username"
fi

# 7. health check
client -u hank -c -w 0.3 >"$TMP/hank.out"
expect "health check answered" "$TMP/hank.out" "HEALTH status=1"

# 8. frames split across several TCP segments are reassembled
if command -v python3 >/dev/null; then
    python3 - "$PORT" >"$TMP/split.out" <<'EOF'
import socket, struct, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
name = b"ivan"
payload = bytes(16) + bytes([len(name)]) + name
frame = struct.pack(">BBHHB", 1, 0, 0, len(payload), 1) + payload
for b in frame:
    s.send(bytes([b])); time.sleep(0.005)
s.settimeout(2)
hdr = s.recv(7)
print("type=%d" % hdr[6])
EOF
    expect "split frames reassembled" "$TMP/split.out" "type=16"
fi

# 9. server is still alive after all of the above
if kill -0 "$SERVER_PID" 2>/dev/null; then pass "server still running"; else fail "server still running"; fi

if [ "$FAILED" -ne 0 ]; then
    echo "--- server log ---"; cat "$TMP/server.log"
    exit 1
fi
echo "all tests passed"
