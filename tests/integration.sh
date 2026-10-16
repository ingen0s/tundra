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

# 1. lobby chat between two users, join events
client -u bob -w 2 >"$TMP/bob.out" &
BOB=$!
sleep 0.3
client -u alice -m "hello bob" -w 0.3 >"$TMP/alice.out"
wait $BOB
expect "register returns a session id" "$TMP/alice.out" "SESSION "
expect "welcome message" "$TMP/alice.out" "SYSTEM Welcome, alice!"
expect "new users auto-join #lobby" "$TMP/alice.out" "EVENT joined #lobby alice"
expect "join event reaches other members" "$TMP/bob.out" "EVENT joined #lobby alice"
expect "sender gets a message id" "$TMP/alice.out" "SENT "
expect "lobby message is delivered" "$TMP/bob.out" "#lobby alice hello bob"
if grep -q "^CHAT .* alice hello bob" "$TMP/alice.out"; then fail "sender does not receive own message"; else pass "sender does not receive own message"; fi

# 2. ordering of several messages
client -u erin -w 1.5 >"$TMP/erin.out" &
ERIN=$!
sleep 0.3
client -u frank -m one -m two -m three -w 0.3 >/dev/null
wait $ERIN
if grep '^CHAT .* frank ' "$TMP/erin.out" | awk '{print $5}' | tr '\n' ' ' | grep -qx 'one two three '; then
    pass "messages arrive in order"
else
    fail "messages arrive in order"; sed 's/^/       /' "$TMP/erin.out"
fi
ids=$(grep '^CHAT .* frank ' "$TMP/erin.out" | awk '{print $2}' | tr '\n' ' ')
set -- $ids
if [ $# -eq 3 ] && [ "$2" -eq $(($1 + 1)) ] && [ "$3" -eq $(($2 + 1)) ]; then pass "message ids increase by one"; else fail "message ids increase by one ($ids)"; fi

# 3. duplicate username is rejected while the first user is connected
client -u gina -w 1.5 >/dev/null &
GINA=$!
sleep 0.3
client -u gina -w 0.3 >"$TMP/dup.out"
wait $GINA
expect "duplicate username rejected" "$TMP/dup.out" "ERROR 1 Username already taken"

# 4. invalid usernames
client -u "has space" -w 0.3 >"$TMP/bad.out"
expect "username with a space rejected" "$TMP/bad.out" "ERROR 8 "
client -u "#room" -w 0.3 >"$TMP/bad2.out"
expect "username starting with # rejected" "$TMP/bad2.out" "ERROR 8 "

# 5. disconnect, miss messages, reconnect with session id, get the backlog
client -u carol -w 0 >"$TMP/carol1.out"
CAROL_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/carol1.out")"
sleep 0.2
client -u dave -m "while you were out" -t carol -m "a dm for carol" -w 0.3 >/dev/null
client -u carol -r "$CAROL_ID" -w 0.5 >"$TMP/carol2.out"
expect "resume is recognised" "$TMP/carol2.out" "SYSTEM Welcome back, carol!"
expect "resume keeps the same session id" "$TMP/carol2.out" "SESSION $CAROL_ID"
expect "buffered room message delivered on resume" "$TMP/carol2.out" "#lobby dave while you were out"
expect "buffered direct message delivered on resume" "$TMP/carol2.out" "@carol dave a dm for carol"
if grep -q "EVENT joined #lobby carol" "$TMP/carol2.out"; then fail "resume does not re-join rooms"; else pass "resume does not re-join rooms"; fi

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

# 8. rooms: join, room-scoped delivery, who, list, leave
client -u kim -j "#dev" -w 2 >"$TMP/kim.out" &
KIM=$!
client -u lee -w 2 >"$TMP/lee.out" &
LEE=$!
sleep 0.3
client -u max -j "#dev" -t "#dev" -m "dev only" -W "#dev" -R -L "#dev" -w 0.4 >"$TMP/max.out"
wait $KIM $LEE
expect "joining creates the room" "$TMP/kim.out" "EVENT joined #dev kim"
expect "members see others join" "$TMP/kim.out" "EVENT joined #dev max"
expect "room message reaches members" "$TMP/kim.out" "#dev max dev only"
if grep -q "dev only" "$TMP/lee.out"; then fail "room message stays in the room"; else pass "room message stays in the room"; fi
expect "who lists members" "$TMP/max.out" "WHO #dev kim 1"
expect "room list includes the room" "$TMP/max.out" "ROOM #dev 2"
expect "leave is announced" "$TMP/kim.out" "EVENT left #dev max"
expect "leaver gets confirmation" "$TMP/max.out" "EVENT left #dev max"

# 9. room errors
client -u nia -t "#nowhere" -m "hi" -j "Bad Name" -L "#lobby" -t "#lobby" -m "after leaving" -t nobody -m "hi" -w 0.4 >"$TMP/nia.out"
expect "send to unknown room" "$TMP/nia.out" "ERROR 6 No such room"
expect "invalid room name" "$TMP/nia.out" "ERROR 8 Invalid room name"
expect "send after leaving" "$TMP/nia.out" "ERROR 7 "
expect "dm to unknown user" "$TMP/nia.out" "ERROR 6 No such user"

# 10. direct messages go only to the recipient
client -u olga -w 1.5 >"$TMP/olga.out" &
OLGA=$!
client -u pete -w 1.5 >"$TMP/pete.out" &
PETE=$!
sleep 0.3
client -u quinn -t olga -m "just for you" -w 0.3 >/dev/null
wait $OLGA $PETE
expect "dm delivered to recipient" "$TMP/olga.out" "@olga quinn just for you"
if grep -q "just for you" "$TMP/pete.out"; then fail "dm not seen by others"; else pass "dm not seen by others"; fi

# 11. history
client -u rob -j "#hist" -t "#hist" -m first -m second -m third -w 0.2 >/dev/null
client -u sam -j "#hist" -y "#hist" -w 0.4 >"$TMP/sam.out"
if grep '^HIST ' "$TMP/sam.out" | awk '{print $5}' | tr '\n' ' ' | grep -qx 'first second third '; then
    pass "history returns room messages oldest first"
else
    fail "history returns room messages oldest first"; sed 's/^/       /' "$TMP/sam.out"
fi
client -u tia -y "#hist" -w 0.3 >"$TMP/tia.out"
expect "history requires membership" "$TMP/tia.out" "ERROR 7 "

# 12. frames split across several TCP segments are reassembled
if command -v python3 >/dev/null; then
    python3 - "$PORT" >"$TMP/split.out" <<'EOF2'
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
EOF2
    expect "split frames reassembled" "$TMP/split.out" "type=16"
fi

# 13. server is still alive after all of the above
if kill -0 "$SERVER_PID" 2>/dev/null; then pass "server still running"; else fail "server still running"; fi

if [ "$FAILED" -ne 0 ]; then
    echo "--- server log ---"; cat "$TMP/server.log"
    exit 1
fi
echo "all tests passed"
