#!/usr/bin/env bash
# End-to-end tests: starts servers on spare ports and drives them with the
# test client and raw frames (tests/tundra_raw.py). Run via `make test`, or
# `make asan && ./tests/integration.sh` to run under the sanitizers; any
# sanitizer report or unclean server exit fails the run.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/build/tundra_chat_server"
CLIENT="$ROOT/build/tundra_client"
# below the usual ephemeral range (32768+) so client sockets cannot collide
BASE_PORT="${TUNDRA_TEST_PORT:-$((20000 + RANDOM % 10000))}"
TMP="$(mktemp -d)"
FAILED=0
SERVER_PIDS=()
export PYTHONPATH="$ROOT/tests"
export ASAN_OPTIONS="${ASAN_OPTIONS:-verify_asan_link_order=0}"

cleanup() {
    for pid in "${SERVER_PIDS[@]}"; do kill "$pid" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT

pass() { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; FAILED=1; }

expect() { # name file pattern
    if grep -qF -- "$3" "$2"; then pass "$1"; else fail "$1 (missing: $3)"; sed 's/^/       /' "$2"; fi
}
expect_not() { # name file pattern
    if grep -qF -- "$3" "$2"; then fail "$1 (unexpected: $3)"; sed 's/^/       /' "$2"; else pass "$1"; fi
}

# Waits until FILE contains PATTERN (default timeout 5s).
wait_line() {
    local file="$1" pattern="$2" tries=$(( ${3:-5} * 20 ))
    while [ "$tries" -gt 0 ]; do
        grep -qF -- "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.05
        tries=$((tries - 1))
    done
    echo "timed out waiting for '$pattern' in $file" >&2
    return 1
}

# start_server NAME PORT [server args...]
start_server() {
    local name="$1" port="$2"
    shift 2
    "$SERVER" -p "$port" "$@" 2>"$TMP/$name.log" &
    SERVER_PIDS+=($!)
    eval "${name}_PID=$!"
    wait_line "$TMP/$name.log" "listening on port" || { cat "$TMP/$name.log"; exit 1; }
}

# stop_server NAME: SIGTERM, then require a clean exit and no sanitizer output.
stop_server() {
    local name="$1" pid
    eval "pid=\$${name}_PID"
    kill -TERM "$pid" 2>/dev/null
    wait "$pid"
    local status=$?
    if [ "$status" -eq 0 ]; then pass "$name server exits cleanly"; else fail "$name server exits cleanly (status $status)"; fi
    if grep -qE "runtime error|Sanitizer|LeakSanitizer" "$TMP/$name.log"; then
        fail "$name server has no sanitizer reports"; sed 's/^/       /' "$TMP/$name.log"
    else
        pass "$name server has no sanitizer reports"
    fi
}

PORT=$BASE_PORT
client() { "$CLIENT" -p "$PORT" "$@"; }
raw() { python3 - "$PORT" "$@"; }

start_server main "$PORT"

# 1. broadcast between two users, join notice
client -u bob -w 3 >"$TMP/bob.out" &
BOB=$!
wait_line "$TMP/bob.out" "SYSTEM Welcome, bob!"
client -u alice -m "hello bob" -w 0.3 >"$TMP/alice.out"
wait $BOB
expect "register returns a session id" "$TMP/alice.out" "SESSION "
expect "welcome message" "$TMP/alice.out" "SYSTEM Welcome, alice!"
expect "join notice reaches other users" "$TMP/bob.out" "SYSTEM alice has joined the chat."
expect "chat message is broadcast" "$TMP/bob.out" "MSG alice hello bob"
expect_not "sender does not receive own message" "$TMP/alice.out" "MSG alice"

# 2. ordering of several messages
client -u erin -w 3 >"$TMP/erin.out" &
ERIN=$!
wait_line "$TMP/erin.out" "SYSTEM Welcome, erin!"
client -u frank -m one -m two -m three -w 0.3 >/dev/null
wait $ERIN
if grep '^MSG frank' "$TMP/erin.out" | awk '{print $3}' | tr '\n' ' ' | grep -qx 'one two three '; then
    pass "messages arrive in order"
else
    fail "messages arrive in order"; sed 's/^/       /' "$TMP/erin.out"
fi

# 3. duplicate username is rejected while the first user is connected
client -u gina -w 3 >"$TMP/gina.out" &
GINA=$!
wait_line "$TMP/gina.out" "SYSTEM Welcome, gina!"
client -u gina -w 0.3 >"$TMP/dup.out"
wait $GINA
expect "duplicate username rejected" "$TMP/dup.out" "ERROR 1 Username already taken"

# 4. invalid username
client -u "has space" -w 0.3 >"$TMP/bad.out"
expect "invalid username rejected" "$TMP/bad.out" "ERROR 2 "

# 5. disconnect, miss messages, reconnect with session id, get the backlog
client -u carol -w 0 >"$TMP/carol1.out"
CAROL_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/carol1.out")"
client -u dave -m "while you were out" -m "second one" -w 0.3 >/dev/null
client -u carol -r "$CAROL_ID" -w 0.5 >"$TMP/carol2.out"
expect "resume is recognised" "$TMP/carol2.out" "SYSTEM Welcome back, carol!"
expect "resume keeps the same session id" "$TMP/carol2.out" "SESSION $CAROL_ID"
expect "buffered message 1 delivered on resume" "$TMP/carol2.out" "MSG dave while you were out"
expect "buffered message 2 delivered on resume" "$TMP/carol2.out" "MSG dave second one"

# 6. a session id cannot be used under a different name, and unknown ids are refused
client -u mallory -r "$CAROL_ID" -w 0.3 >"$TMP/mallory.out"
expect "session id is bound to its username" "$TMP/mallory.out" "ERROR 3 "
client -u nobody -r 00000000-0000-4000-8000-000000000001 -w 0.3 >"$TMP/unknown.out"
expect "unknown session id is refused" "$TMP/unknown.out" "ERROR 3 "

# 7. resuming while the old connection is still open replaces it
client -u hugo -w 4 >"$TMP/hugo1.out" &
HUGO=$!
wait_line "$TMP/hugo1.out" "SESSION "
HUGO_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/hugo1.out")"
client -u hugo -r "$HUGO_ID" -w 0.3 >"$TMP/hugo2.out"
wait $HUGO
expect "takeover resumes the session" "$TMP/hugo2.out" "SYSTEM Welcome back, hugo!"
expect "takeover closes the old connection" "$TMP/hugo1.out" "CLOSED"

# 8. backlog keeps the newest 50 messages and says how many were dropped
client -u iris -w 0 >"$TMP/iris1.out"
IRIS_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/iris1.out")"
args=()
for i in $(seq 1 60); do args+=(-m "m$i"); done
client -u jack "${args[@]}" -w 0.3 >/dev/null
client -u iris -r "$IRIS_ID" -w 0.5 >"$TMP/iris2.out"
count=$(grep -c '^MSG jack ' "$TMP/iris2.out")
first=$(grep -m1 '^MSG jack ' "$TMP/iris2.out" | awk '{print $3}')
last=$(grep '^MSG jack ' "$TMP/iris2.out" | tail -1 | awk '{print $3}')
if [ "$count" -eq 50 ] && [ "$first" = "m11" ] && [ "$last" = "m60" ]; then
    pass "backlog keeps the newest 50"
else
    fail "backlog keeps the newest 50 (got $count, $first..$last)"
fi
expect "dropped messages are reported" "$TMP/iris2.out" "SYSTEM 10 buffered message(s) were dropped while you were away"

# 9. a backlog larger than the output limit is replayed in full, not cut off
client -u kate -w 0 >"$TMP/kate1.out"
KATE_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/kate1.out")"
big="$(head -c 60000 /dev/zero | tr '\0' 'x')"
args=()
for i in $(seq 1 15); do args+=(-m "big$i-$big"); done
client -u leo "${args[@]}" -w 0.5 >/dev/null
client -u kate -r "$KATE_ID" -w 2 >"$TMP/kate2.out"
count=$(grep -c '^MSG leo big' "$TMP/kate2.out")
if [ "$count" -eq 15 ]; then pass "large backlog replayed in full"; else fail "large backlog replayed in full (got $count of 15)"; fi
expect_not "large replay keeps the connection" "$TMP/kate2.out" "CLOSED"

# 10. health check
client -u hank -c -w 0.3 >"$TMP/hank.out"
expect "health check answered" "$TMP/hank.out" "HEALTH status=1"

# 11. malformed and out-of-order frames
raw >"$TMP/raw.out" <<'EOF'
import sys
from tundra_raw import *
port = int(sys.argv[1])

c = Conn(port)
c.send(frame(0x02, s16(b"hi")))
f = c.recv_frame()
print("before-register", error_code(f[1]) if f and f[0] == 0x15 else f)

c.send(register(b"rawuser"))
c.wait_for(0x10)
c.send(frame(0x02, s16(b"hi") + b"!"))
f = c.wait_for(0x15)
print("trailing-byte", error_code(f[1]) if f else None)
c.send(frame(0x02, b"\x00\x09hi"))
f = c.wait_for(0x15)
print("length-past-end", error_code(f[1]) if f else None)
c.send(frame(0x03, s8(b"") + s16(b"cmd")))
f = c.wait_for(0x15)
print("empty-agent", error_code(f[1]) if f else None)
c.send(frame(0x03, s8(b"bad name") + s16(b"cmd")))
f = c.wait_for(0x15)
print("agent-with-space", error_code(f[1]) if f else None)
c.send(frame(0x02, s16(b"a" * 65493)))
f = c.wait_for(0x15)
print("too-long", f[1][4:].decode() if f else None)
c.send(frame(0x02, s16(b"a" * 65492)))
f = c.recv_frame()
print("max-length", "error" if f and f[0] == 0x15 else "accepted")
c.send(frame(0x7e))
f = c.wait_for(0x15)
print("unknown-type", error_code(f[1]) if f else None)

v = Conn(port)
v.send(frame(0x01, bytes(16) + s8(b"v2user"), version=2))
f = v.recv_frame()
print("bad-version", error_code(f[1]) if f and f[0] == 0x15 else f, "closed" if v.closed() else "open")
EOF
expect "frames before REGISTER are refused" "$TMP/raw.out" "before-register 5"
expect "trailing bytes rejected" "$TMP/raw.out" "trailing-byte 2"
expect "length past end rejected" "$TMP/raw.out" "length-past-end 2"
expect "empty agent name rejected" "$TMP/raw.out" "empty-agent 2"
expect "agent name with space rejected" "$TMP/raw.out" "agent-with-space 2"
expect "text over 65492 bytes rejected" "$TMP/raw.out" "too-long Message too long"
expect "text of 65492 bytes accepted" "$TMP/raw.out" "max-length accepted"
expect "unknown type rejected" "$TMP/raw.out" "unknown-type 2"
expect "bad version gets an error and is closed" "$TMP/raw.out" "bad-version 2 closed"

# 12. agent command broadcast format
client -u mona -w 3 >"$TMP/mona.out" &
MONA=$!
wait_line "$TMP/mona.out" "SYSTEM Welcome, mona!"
raw >/dev/null <<'EOF'
import sys
from tundra_raw import *
c = Conn(int(sys.argv[1]))
c.send(register(b"ned"))
c.wait_for(0x10)
c.send(frame(0x03, s8(b"planner") + s16(b"summarize today")))
c.recv_frame()
EOF
wait $MONA
expect "agent command broadcast" "$TMP/mona.out" "AGENT ned planner summarize today"

# 13. frames split across several TCP segments are reassembled
raw >"$TMP/split.out" <<'EOF'
import sys, time
from tundra_raw import *
c = Conn(int(sys.argv[1]))
for b in register(b"ivan"):
    c.send(bytes([b])); time.sleep(0.005)
f = c.recv_frame()
print("type=%d" % f[0])
EOF
expect "split frames reassembled" "$TMP/split.out" "type=16"

# 14. a full session table evicts the oldest detached session instead of refusing
raw >"$TMP/fill.out" <<'EOF'
import sys
from tundra_raw import *
port = int(sys.argv[1])
ok = 0
for i in range(1000):
    c = Conn(port)
    c.send(register(b"filler%d" % i))
    f = c.recv_frame()
    if f and f[0] == 0x10:
        ok += 1
    c.sock.close()
c = Conn(port)
c.send(register(b"latecomer"))
f = c.recv_frame()
print("fillers", ok, "latecomer", "welcome" if f and f[0] == 0x10 else ("error %d" % error_code(f[1]) if f else "none"))
EOF
expect "full session table makes room for new users" "$TMP/fill.out" "latecomer welcome"

stop_server main

# 15. expiry: a detached session is released after the grace period
PORT=$((BASE_PORT + 1))
start_server expiry "$PORT" -g 1
client -u owen -w 5 >"$TMP/owen.out" &
OWEN=$!
wait_line "$TMP/owen.out" "SYSTEM Welcome, owen!"
client -u pia -w 0 >/dev/null
wait_line "$TMP/owen.out" "SYSTEM pia has left the chat." 6
expect "expired session announces it left" "$TMP/owen.out" "SYSTEM pia has left the chat."
client -u pia -w 0.3 >"$TMP/pia2.out"
expect "expired username can be registered again" "$TMP/pia2.out" "SYSTEM Welcome, pia!"
wait $OWEN
stop_server expiry

if [ "$FAILED" -ne 0 ]; then
    for log in "$TMP"/*.log; do echo "--- $(basename "$log") ---"; tail -40 "$log"; done
    exit 1
fi
echo "all tests passed"
