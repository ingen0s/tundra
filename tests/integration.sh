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

# 1. lobby chat between two users, join events
client -u bob -w 3 >"$TMP/bob.out" &
BOB=$!
wait_line "$TMP/bob.out" "SYSTEM Welcome, bob!"
client -u alice -m "hello bob" -w 0.3 >"$TMP/alice.out"
wait $BOB
expect "register returns a session id" "$TMP/alice.out" "SESSION "
expect "welcome message" "$TMP/alice.out" "SYSTEM Welcome, alice!"
expect "new users auto-join #lobby" "$TMP/alice.out" "EVENT joined #lobby alice"
expect "join event reaches other members" "$TMP/bob.out" "EVENT joined #lobby alice"
expect "sender gets a message id" "$TMP/alice.out" "SENT "
expect "lobby message is delivered" "$TMP/bob.out" "#lobby alice hello bob"
expect_not "sender does not receive own message" "$TMP/alice.out" "alice hello bob"

# 2. ordering and ids of several messages
client -u erin -w 3 >"$TMP/erin.out" &
ERIN=$!
wait_line "$TMP/erin.out" "SYSTEM Welcome, erin!"
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
client -u gina -w 3 >"$TMP/gina.out" &
GINA=$!
wait_line "$TMP/gina.out" "SYSTEM Welcome, gina!"
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
client -u dave -m "while you were out" -t carol -m "a dm for carol" -w 0.3 >/dev/null
client -u carol -r "$CAROL_ID" -w 0.5 >"$TMP/carol2.out"
expect "resume is recognised" "$TMP/carol2.out" "SYSTEM Welcome back, carol!"
expect "resume keeps the same session id" "$TMP/carol2.out" "SESSION $CAROL_ID"
expect "buffered room message delivered on resume" "$TMP/carol2.out" "#lobby dave while you were out"
expect "buffered direct message delivered on resume" "$TMP/carol2.out" "@carol dave a dm for carol"
expect_not "resume does not re-join rooms" "$TMP/carol2.out" "EVENT joined #lobby carol"

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

# 8. backlog keeps the newest 50 frames and says how many were dropped
#    (iris's backlog gets jack's join event, then 60 messages)
client -u iris -w 0 >"$TMP/iris1.out"
IRIS_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/iris1.out")"
args=()
for i in $(seq 1 60); do args+=(-m "m$i"); done
client -u jack "${args[@]}" -w 0.3 >/dev/null
client -u iris -r "$IRIS_ID" -w 0.5 >"$TMP/iris2.out"
count=$(grep -c '^CHAT .* jack ' "$TMP/iris2.out")
first=$(grep -m1 '^CHAT .* jack ' "$TMP/iris2.out" | awk '{print $5}')
last=$(grep '^CHAT .* jack ' "$TMP/iris2.out" | tail -1 | awk '{print $5}')
if [ "$count" -eq 50 ] && [ "$first" = "m11" ] && [ "$last" = "m60" ]; then
    pass "backlog keeps the newest 50"
else
    fail "backlog keeps the newest 50 (got $count, $first..$last)"
fi
expect "dropped frames are reported" "$TMP/iris2.out" "SYSTEM 11 buffered message(s) were dropped while you were away"

# 9. a backlog larger than the output limit is replayed in full, not cut off
client -u kate -w 0 >"$TMP/kate1.out"
KATE_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/kate1.out")"
big="$(head -c 60000 /dev/zero | tr '\0' 'x')"
args=()
for i in $(seq 1 15); do args+=(-m "big$i-$big"); done
client -u leo "${args[@]}" -w 0.5 >/dev/null
client -u kate -r "$KATE_ID" -w 2 >"$TMP/kate2.out"
count=$(grep -c '^CHAT .* leo big' "$TMP/kate2.out")
if [ "$count" -eq 15 ]; then pass "large backlog replayed in full"; else fail "large backlog replayed in full (got $count of 15)"; fi
expect_not "large replay keeps the connection" "$TMP/kate2.out" "CLOSED"

# 10. health check
client -u hank -c -w 0.3 >"$TMP/hank.out"
expect "health check answered" "$TMP/hank.out" "HEALTH status=1"

# 11. rooms: join, room-scoped delivery, who, list, leave
client -u kim -j "#dev" -w 3 >"$TMP/kim.out" &
KIM=$!
client -u lee -w 3 >"$TMP/lee.out" &
LEE=$!
wait_line "$TMP/kim.out" "EVENT joined #dev kim"
wait_line "$TMP/lee.out" "SYSTEM Welcome, lee!"
client -u max -j "#dev" -t "#dev" -m "dev only" -W "#dev" -R -L "#dev" -w 0.4 >"$TMP/max.out"
wait $KIM $LEE
expect "members see others join" "$TMP/kim.out" "EVENT joined #dev max"
expect "room message reaches members" "$TMP/kim.out" "#dev max dev only"
expect_not "room message stays in the room" "$TMP/lee.out" "dev only"
expect "who lists members" "$TMP/max.out" "WHO #dev kim 1"
expect "room list includes the room" "$TMP/max.out" "ROOM #dev 2"
expect "leave is announced" "$TMP/kim.out" "EVENT left #dev max"
expect "leaver gets confirmation" "$TMP/max.out" "EVENT left #dev max"

# 12. room errors
client -u nia -t "#nowhere" -m "hi" -j "Bad Name" -L "#lobby" -t "#lobby" -m "after leaving" -t nobody -m "hi" -w 0.4 >"$TMP/nia.out"
expect "send to unknown room" "$TMP/nia.out" "ERROR 6 No such room"
expect "invalid room name" "$TMP/nia.out" "ERROR 8 Invalid room name"
expect "send after leaving" "$TMP/nia.out" "ERROR 7 "
expect "dm to unknown user" "$TMP/nia.out" "ERROR 6 No such user"

# 13. direct messages go only to the recipient
client -u olga -w 3 >"$TMP/olga.out" &
OLGA=$!
client -u pete -w 3 >"$TMP/pete.out" &
PETE=$!
wait_line "$TMP/olga.out" "SYSTEM Welcome, olga!"
wait_line "$TMP/pete.out" "SYSTEM Welcome, pete!"
client -u quinn -t olga -m "just for you" -w 0.3 >/dev/null
wait $OLGA $PETE
expect "dm delivered to recipient" "$TMP/olga.out" "@olga quinn just for you"
expect_not "dm not seen by others" "$TMP/pete.out" "just for you"

# 14. history
client -u rob -j "#hist" -t "#hist" -m first -m second -m third -w 0.2 >/dev/null
client -u sam -j "#hist" -y "#hist" -w 0.4 >"$TMP/sam.out"
if grep '^HIST ' "$TMP/sam.out" | awk '{print $5}' | tr '\n' ' ' | grep -qx 'first second third '; then
    pass "history returns room messages oldest first"
else
    fail "history returns room messages oldest first"; sed 's/^/       /' "$TMP/sam.out"
fi
client -u tia -y "#hist" -w 0.3 >"$TMP/tia.out"
expect "history requires membership" "$TMP/tia.out" "ERROR 7 "

# 15. malformed and out-of-order frames
raw >"$TMP/raw.out" <<'PY'
import sys
from tundra_raw import *
port = int(sys.argv[1])
MAX_TEXT = 65535 - 35 - 2 - 85

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
c.send(frame(0x02, s16(b"a" * (MAX_TEXT + 1))))
f = c.wait_for(0x15)
print("too-long", f[1][4:].decode() if f else None)
c.send(frame(0x02, s16(b"a" * MAX_TEXT)))
f = c.wait_for(0x18)
print("max-length", "accepted" if f else "error")
c.send(frame(0x7e))
f = c.wait_for(0x15)
print("unknown-type", error_code(f[1]) if f else None)
c.send(frame(0x17, bytes(30)))
f = c.wait_for(0x15)
print("server-type-from-client", error_code(f[1]) if f else None)

v = Conn(port)
v.send(frame(0x01, bytes(16) + s8(b"v2user"), version=2))
f = v.recv_frame()
print("bad-version", error_code(f[1]) if f and f[0] == 0x15 else f, "closed" if v.closed() else "open")
PY
expect "frames before REGISTER are refused" "$TMP/raw.out" "before-register 5"
expect "trailing bytes rejected" "$TMP/raw.out" "trailing-byte 2"
expect "length past end rejected" "$TMP/raw.out" "length-past-end 2"
expect "empty agent name rejected" "$TMP/raw.out" "empty-agent 2"
expect "agent name with space rejected" "$TMP/raw.out" "agent-with-space 2"
expect "text over the limit rejected" "$TMP/raw.out" "too-long Message too long"
expect "text at the limit accepted" "$TMP/raw.out" "max-length accepted"
expect "unknown type rejected" "$TMP/raw.out" "unknown-type 2"
expect "server-only type from a client rejected" "$TMP/raw.out" "server-type-from-client 2"
expect "bad version gets an error and is closed" "$TMP/raw.out" "bad-version 2 closed"

# 16. agent command broadcast format
client -u mona -w 3 >"$TMP/mona.out" &
MONA=$!
wait_line "$TMP/mona.out" "SYSTEM Welcome, mona!"
raw >/dev/null <<'PY'
import sys
from tundra_raw import *
c = Conn(int(sys.argv[1]))
c.send(register(b"ned"))
c.wait_for(0x10)
c.send(frame(0x03, s8(b"planner") + s16(b"summarize today")))
c.recv_frame()
PY
wait $MONA
expect "agent command broadcast" "$TMP/mona.out" "AGENT ned planner summarize today"

# 17. frames split across several TCP segments are reassembled
raw >"$TMP/split.out" <<'PY'
import sys, time
from tundra_raw import *
c = Conn(int(sys.argv[1]))
for b in register(b"ivan"):
    c.send(bytes([b])); time.sleep(0.005)
f = c.recv_frame()
print("type=%d" % f[0])
PY
expect "split frames reassembled" "$TMP/split.out" "type=16"

# 18. a full session table evicts the oldest detached session instead of refusing
raw >"$TMP/fill.out" <<'PY'
import sys
from tundra_raw import *
port = int(sys.argv[1])
ok = 0
for i in range(1000):
    c = Conn(port)
    c.send(register(b"filler%d" % i))
    if c.wait_for(0x10):
        ok += 1
    c.sock.close()
c = Conn(port)
c.send(register(b"latecomer"))
print("fillers", ok, "latecomer", "welcome" if c.wait_for(0x10) else "refused")
PY
expect "full session table makes room for new users" "$TMP/fill.out" "latecomer welcome"

# 18c. review regressions: history limits, recreated rooms, resume order
raw >"$TMP/roomfix.out" <<'PY'
import sys, struct, time
from tundra_raw import *
port = int(sys.argv[1])
MAX_TEXT = 65535 - 35 - 2 - 85
room = b"#" + b"r" * 31
user = b"u" * 32

# a maximum-size message with maximum-length names still fits in HISTORY
c = Conn(port)
c.send(register(user))
c.wait_for(0x10)
c.send(frame(0x06, s8(room)))
c.wait_for(0x1A)
c.send(frame(0x08, bytes([0]) + s8(room) + s16(b"x" * MAX_TEXT)))
f = c.wait_for(0x18)
print("max-message-sent", f is not None)
c.send(frame(0x0B, s8(room) + struct.pack(">QH", 0, 10)))
f = c.wait_for(0x1B)
print("max-message-in-history", struct.unpack(">H", f[1][33:35])[0] if f else None)

# a deleted and recreated room does not show the old room's messages
a = Conn(port)
a.send(register(b"oldroomer"))
a.wait_for(0x10)
a.send(frame(0x06, s8(b"#reuse")))
a.wait_for(0x1A)
a.send(frame(0x08, bytes([0]) + s8(b"#reuse") + s16(b"old secret")))
a.wait_for(0x18)
a.send(frame(0x07, s8(b"#reuse")))  # last member leaves: room deleted
a.wait_for(0x1A)
b = Conn(port)
b.send(register(b"newroomer"))
b.wait_for(0x10)
b.send(frame(0x06, s8(b"#reuse")))
b.wait_for(0x1A)
b.send(frame(0x0B, s8(b"#reuse") + struct.pack(">QH", 0, 10)))
f = b.wait_for(0x1B)
print("recreated-room-history", struct.unpack(">H", f[1][7:9])[0] if f else None)

# resume order: welcome, then the drop notice, then the backlog
r = Conn(port)
r.send(register(b"orderly"))
sid = r.wait_for(0x10)[1][:16]
r.sock.close()
time.sleep(0.3)
t = Conn(port)
t.send(register(b"orderflood"))
t.wait_for(0x10)
for i in range(55):
    t.send(frame(0x02, s16(b"o%d" % i)))
while t.wait_for(0x18, 0.3):
    pass
r = Conn(port)
r.send(register(b"orderly", sid))
kinds = []
while True:
    f = r.recv_frame()
    if f is None:
        break
    if f[0] == 0x10:
        kinds.append("welcome" if b"Welcome back" in f[1] else ("notice" if b"dropped" in f[1] else "system"))
    elif f[0] == 0x17:
        if not kinds or kinds[-1] != "chat":
            kinds.append("chat")
print("resume-order", " ".join(kinds[:3]))
PY
expect "a maximum-size message is accepted" "$TMP/roomfix.out" "max-message-sent True"
expect "a maximum-size message fits in HISTORY" "$TMP/roomfix.out" "max-message-in-history 1"
expect "a recreated room has no old history" "$TMP/roomfix.out" "recreated-room-history 0"
expect "resume sends welcome, drop notice, then backlog" "$TMP/roomfix.out" "resume-order welcome notice chat"

stop_server main

# 19. expiry: a detached session is released after the grace period
PORT=$((BASE_PORT + 1))
start_server expiry "$PORT" -g 1
client -u owen -w 5 >"$TMP/owen.out" &
OWEN=$!
wait_line "$TMP/owen.out" "SYSTEM Welcome, owen!"
client -u pia -w 0 >/dev/null
wait_line "$TMP/owen.out" "EVENT left #lobby pia" 6
expect "expired session leaves its rooms" "$TMP/owen.out" "EVENT left #lobby pia"
client -u pia -w 0.3 >"$TMP/pia2.out"
expect "expired username can be registered again" "$TMP/pia2.out" "SYSTEM Welcome, pia!"
wait $OWEN
stop_server expiry

if [ "$FAILED" -ne 0 ]; then
    for log in "$TMP"/*.log; do echo "--- $(basename "$log") ---"; tail -40 "$log"; done
    exit 1
fi
echo "all tests passed"
