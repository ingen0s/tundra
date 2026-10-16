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

# 8. catch-up is not limited to a fixed backlog: all 60 missed messages arrive
client -u iris -w 0 >"$TMP/iris1.out"
IRIS_ID="$(awk '/^SESSION/{print $2; exit}' "$TMP/iris1.out")"
args=()
for i in $(seq 1 60); do args+=(-m "m$i"); done
client -u jack "${args[@]}" -w 0.3 >/dev/null
client -u iris -r "$IRIS_ID" -w 0.5 >"$TMP/iris2.out"
count=$(grep -c '^CHAT .* jack ' "$TMP/iris2.out")
first=$(grep -m1 '^CHAT .* jack ' "$TMP/iris2.out" | awk '{print $5}')
last=$(grep '^CHAT .* jack ' "$TMP/iris2.out" | tail -1 | awk '{print $5}')
if [ "$count" -eq 60 ] && [ "$first" = "m1" ] && [ "$last" = "m60" ]; then
    pass "resume replays every missed message in order"
else
    fail "resume replays every missed message in order (got $count, $first..$last)"
fi
expect "events buffered while away are delivered" "$TMP/iris2.out" "EVENT joined #lobby jack"

# 8b. ACKs and resume points
raw >"$TMP/ack.out" <<'PY'
import sys, struct, time
from tundra_raw import *
port = int(sys.argv[1])

def chat_id(p):
    return struct.unpack(">Q", p[:8])[0]

def chats(conn, n):
    ids = []
    while len(ids) < n:
        f = conn.wait_for(0x17)
        if not f:
            break
        ids.append(chat_id(f[1]))
    return ids

listener = Conn(port)
listener.send(register(b"ackuser"))
sid = listener.wait_for(0x10)[1][:16]
talker = Conn(port)
talker.send(register(b"acktalker"))
talker.wait_for(0x10)
for i in range(5):
    talker.send(frame(0x02, s16(b"a%d" % i)))
got = chats(listener, 5)
print("received", len(got))
# acknowledge only the first two, then drop the connection
listener.send(frame(0x0A, struct.pack(">Q", got[1])))
listener.sock.close()

# resume without a resume point: the server continues after the last ACK
r = Conn(port)
r.send(register(b"ackuser", sid))
r.wait_for(0x10)
again = chats(r, 3)
print("after-ack", " ".join(str(i - got[0]) for i in again))
r.sock.close()

# resume with an explicit point: replay everything after it
r = Conn(port)
r.send(frame(0x01, sid + s8(b"ackuser") + bytes([0x01]) + struct.pack(">H", 8) + struct.pack(">Q", got[0])))
r.wait_for(0x10)
again = chats(r, 4)
print("after-point", " ".join(str(i - got[0]) for i in again))
r.sock.close()

# without ACKs, the server continues after what it fully wrote
q = Conn(port)
q.send(register(b"noacker"))
qsid = q.wait_for(0x10)[1][:16]
talker.send(frame(0x02, s16(b"before")))
q.wait_for(0x17)
q.sock.close()
time.sleep(0.3)  # let the server see the disconnect; bytes written to a dying socket count as written
talker.send(frame(0x02, s16(b"after")))
talker.wait_for(0x18)
q = Conn(port)
q.send(register(b"noacker", qsid))
q.wait_for(0x10)
f = q.wait_for(0x17, 1.0)
print("no-ack-resume", f[1][-5:].decode() if f else None, "dup" if q.wait_for(0x17, 0.3) else "nodup")
PY
expect "listener got the messages" "$TMP/ack.out" "received 5"
expect "resume continues after the last ACK" "$TMP/ack.out" "after-ack 2 3 4"
expect "resume point replays after it" "$TMP/ack.out" "after-point 1 2 3 4"
expect "without ACKs resume continues after what was written" "$TMP/ack.out" "no-ack-resume after nodup"

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

# resume order: the welcome comes before the catch-up
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
expect "resume sends the welcome, then the catch-up" "$TMP/roomfix.out" "resume-order welcome chat"
# 18b. catching up past the retained log says so, and still delivers what is held
raw >"$TMP/gap.out" <<'PY'
import sys
from tundra_raw import *
port = int(sys.argv[1])
z = Conn(port)
z.send(register(b"zed"))
sid = z.wait_for(0x10)[1][:16]
z.sock.close()
t = Conn(port)
t.send(register(b"flooder"))
t.wait_for(0x10)
for i in range(10050):
    t.send(frame(0x02, s16(b"f%d" % i)))
while t.wait_for(0x18, 0.5):
    pass
z = Conn(port)
z.send(register(b"zed", sid))
notice = False
last = None
count = 0
while True:
    f = z.recv_frame()
    if f is None:
        break
    if f[0] == 0x10 and b"no longer available" in f[1]:
        notice = True
    if f[0] == 0x17:
        count += 1
        last = f[1][-6:]
print("gap-notice", notice, "count", count, "last", last.decode() if last else None)
PY
expect "gap in retained history is reported" "$TMP/gap.out" "gap-notice True"
expect "catch-up delivers up to the newest message" "$TMP/gap.out" "last f10049"

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

# 18d. catch-up never reaches before the session or the join (short-grace server)
PORT=$((BASE_PORT + 10))
start_server catchup "$PORT" -g 1
raw >"$TMP/catchup2.out" <<'PY'
import sys, struct, time
from tundra_raw import *
port = int(sys.argv[1])

def texts(conn, wait=0.6):
    out = []
    while True:
        f = conn.wait_for(0x17, wait)
        if not f:
            return out
        p = f[1]
        tl = p[17]; sl = p[18 + tl]; ml = struct.unpack(">H", p[19 + tl + sl:21 + tl + sl])[0]
        out.append(p[21 + tl + sl:21 + tl + sl + ml].decode())

old = Conn(port)
old.send(register(b"namesake"))
old.wait_for(0x10)
old.sock.close()
s = Conn(port)
s.send(register(b"dmsender"))
s.wait_for(0x10)
s.send(frame(0x08, bytes([1]) + s8(b"namesake") + s16(b"for the old namesake")))
s.wait_for(0x18)
time.sleep(2.5)  # the old session expires
new = Conn(port)
new.send(register(b"namesake"))
nsid = new.wait_for(0x10)[1][:16]
new.sock.close()
time.sleep(0.2)
new = Conn(port)
new.send(frame(0x01, nsid + s8(b"namesake") + bytes([1]) + struct.pack(">HQ", 8, 0)))  # RESUME_FROM 0
new.wait_for(0x10)
print("new-namesake-gets", texts(new))

# a room joined later does not replay messages from before the join
a = Conn(port)
a.send(register(b"joiner"))
asid = a.wait_for(0x10)[1][:16]
t = Conn(port)
t.send(register(b"roomtalker"))
t.wait_for(0x10)
t.send(frame(0x06, s8(b"#later")))
t.wait_for(0x1A)
t.send(frame(0x08, bytes([0]) + s8(b"#later") + s16(b"before you joined")))
t.wait_for(0x18)
a.send(frame(0x06, s8(b"#later")))
a.wait_for(0x1A)
t.send(frame(0x08, bytes([0]) + s8(b"#later") + s16(b"after you joined")))
t.wait_for(0x18)
texts(a)
a.sock.close()
time.sleep(0.2)
a = Conn(port)
a.send(frame(0x01, asid + s8(b"joiner") + bytes([1]) + struct.pack(">HQ", 8, 0)))
a.wait_for(0x10)
print("joiner-replay", texts(a))

bad = Conn(port)
bad.send(frame(0x01, asid + s8(b"joiner") + bytes([1]) + struct.pack(">H", 3) + b"abc"))
f = bad.wait_for(0x15)
print("bad-resume-from", error_code(f[1]) if f else None)
PY
expect "a reused name gets no direct messages meant for its earlier holder" "$TMP/catchup2.out" "new-namesake-gets []"
expect "catch-up skips room messages from before the join" "$TMP/catchup2.out" "joiner-replay ['after you joined']"
expect "a RESUME_FROM that is not 8 bytes is refused" "$TMP/catchup2.out" "bad-resume-from 2"
stop_server catchup
# 20. token authentication
PORT=$((BASE_PORT + 2))
printf '# test tokens\nalice s3cret\nbot1 t0k\n' >"$TMP/tokens"
start_server auth "$PORT" -t "$TMP/tokens"
client -u alice -w 0.2 >"$TMP/auth1.out"
expect "listed name without a token is refused" "$TMP/auth1.out" "ERROR 9 "
client -u alice -k wrong -w 0.2 >"$TMP/auth2.out"
expect "wrong token is refused" "$TMP/auth2.out" "ERROR 9 "
client -u alice -k s3cret -w 0.2 >"$TMP/auth3.out"
expect "right token is accepted" "$TMP/auth3.out" "SYSTEM Welcome, alice!"
client -u bob -w 0.2 >"$TMP/auth4.out"
expect "unlisted names stay open without -A" "$TMP/auth4.out" "SYSTEM Welcome, bob!"
client -u carl -k newtok -w 0.2 >"$TMP/auth5.out"
expect "unlisted name with a token is still accepted" "$TMP/auth5.out" "SYSTEM Welcome, carl!"
printf 'dora d0ra\n' >>"$TMP/tokens"
kill -HUP "$auth_PID"
wait_line "$TMP/auth.log" "3 user(s) loaded"
client -u dora -w 0.2 >"$TMP/auth6.out"
expect "tokens file is re-read on SIGHUP" "$TMP/auth6.out" "ERROR 9 "

# a bad line rejects the whole reload and keeps the previous tokens
cp "$TMP/tokens" "$TMP/tokens.good"
printf 'alice\n' >"$TMP/tokens"
kill -HUP "$auth_PID"
wait_line "$TMP/auth.log" "keeping the previous tokens"
client -u alice -w 0.2 >"$TMP/auth9.out"
expect "a malformed tokens file fails closed" "$TMP/auth9.out" "ERROR 9 "
: >"$TMP/tokens"
kill -HUP "$auth_PID"
sleep 0.3
client -u alice -w 0.2 >"$TMP/auth10.out"
expect "an empty tokens file fails closed" "$TMP/auth10.out" "ERROR 9 "

# rotating a token ends the session that used the old one
cp "$TMP/tokens.good" "$TMP/tokens"
kill -HUP "$auth_PID"
wait_line "$TMP/auth.log" "3 user(s) loaded"
client -u bot1 -k t0k -w 3 >"$TMP/rot.out" &
ROT=$!
wait_line "$TMP/rot.out" "SYSTEM Welcome, bot1!"
sed -i 's/^bot1 t0k$/bot1 n3w/' "$TMP/tokens"
kill -HUP "$auth_PID"
wait $ROT
expect "rotating a token ends the old session" "$TMP/rot.out" "ERROR 9 Your token is no longer valid"
client -u bot1 -k n3w -w 0.2 >"$TMP/rot2.out"
expect "the new token works right away" "$TMP/rot2.out" "SYSTEM Welcome, bot1!"
stop_server auth

: >"$TMP/empty-tokens"
if "$SERVER" -p $((BASE_PORT + 2)) -t "$TMP/empty-tokens" 2>"$TMP/empty.log"; then
    fail "server refuses to start with an empty tokens file"
else
    pass "server refuses to start with an empty tokens file"
fi

PORT=$((BASE_PORT + 3))
start_server authall "$PORT" -t "$TMP/tokens.good" -A
client -u erin -w 0.2 >"$TMP/auth7.out"
expect "-A refuses unlisted names" "$TMP/auth7.out" "ERROR 9 "
client -u bot1 -k t0k -w 0.2 >"$TMP/auth8.out"
expect "-A accepts listed names with their token" "$TMP/auth8.out" "SYSTEM Welcome, bot1!"
stop_server authall

# 21. persistence: history and ids survive a restart
PORT=$((BASE_PORT + 4))
start_server store1 "$PORT" -d "$TMP/chat.db"
client -u fay -j "#keep" -t "#keep" -m "one" -m "two" -m "three" -w 0.3 >"$TMP/store1.out"
last_id=$(awk '/^SENT/{id=$2} END{print id}' "$TMP/store1.out")
stop_server store1
start_server store2 "$PORT" -d "$TMP/chat.db"
expect "stored messages are loaded on start" "$TMP/store2.log" "loaded 3 recent message(s)"
client -u gus -j "#keep" -y "#keep" -t "#keep" -m "four" -w 0.3 >"$TMP/store2.out"
if grep '^HIST ' "$TMP/store2.out" | awk '{print $5}' | tr '\n' ' ' | grep -qx 'one two three '; then
    pass "history survives a restart"
else
    fail "history survives a restart"; sed 's/^/       /' "$TMP/store2.out"
fi
new_id=$(awk '/^SENT/{print $2}' "$TMP/store2.out")
# ids are reserved in blocks, so after a restart they keep increasing but may skip
if [ -n "$new_id" ] && [ -n "$last_id" ] && [ "$new_id" -gt "$last_id" ]; then
    pass "message ids keep increasing after a restart"
else
    fail "message ids keep increasing after a restart ($last_id then $new_id)"
fi
# paging back with before_id
raw >"$TMP/page.out" <<'PY2'
import sys, struct
from tundra_raw import *
c = Conn(int(sys.argv[1]))
c.send(register(b"hal"))
c.wait_for(0x10)
c.send(frame(0x06, s8(b"#keep")))
c.wait_for(0x1A)
c.send(frame(0x0B, s8(b"#keep") + struct.pack(">QH", 0, 2)))
f = c.wait_for(0x1B)
n = struct.unpack(">H", f[1][6:8])[0]
p = f[1][8:]
texts = []
oldest = None
for _ in range(n):
    ln = struct.unpack(">H", p[:2])[0]; e = p[2:2 + ln]; p = p[2 + ln:]
    mid = struct.unpack(">Q", e[:8])[0]
    oldest = mid if oldest is None else min(oldest, mid)
    texts.append(e)
def text(e):
    # CHAT payload: id(8) ts(8) kind(1) target(str8) sender(str8) text(str16)
    tl = e[17]; sl = e[18 + tl]; ml = struct.unpack(">H", e[19 + tl + sl:21 + tl + sl])[0]
    return e[21 + tl + sl:21 + tl + sl + ml].decode()
print("page1", " ".join(text(e) for e in texts))
c.send(frame(0x0B, s8(b"#keep") + struct.pack(">QH", oldest, 2)))
f = c.wait_for(0x1B)
n = struct.unpack(">H", f[1][6:8])[0]
p = f[1][8:]
texts = []
for _ in range(n):
    ln = struct.unpack(">H", p[:2])[0]; texts.append(p[2:2 + ln]); p = p[2 + ln:]
print("page2", " ".join(text(e) for e in texts))
c.send(frame(0x0B, s8(b"#keep") + struct.pack(">QH", 0xFFFFFFFFFFFFFFFF, 2)))
f = c.wait_for(0x1B)
print("huge-before-id", struct.unpack(">H", f[1][6:8])[0])
PY2
expect "history pages newest first" "$TMP/page.out" "page1 three four"
expect "before_id pages further back" "$TMP/page.out" "page2 one two"
expect "a huge before_id means latest" "$TMP/page.out" "huge-before-id 2"
# a room deleted before a restart still starts empty after it
client -u ivy -j "#gone" -t "#gone" -m "from the old room" -L "#gone" -w 0.3 >/dev/null
stop_server store2
start_server store3 "$PORT" -d "$TMP/chat.db"
client -u jay -j "#gone" -y "#gone" -w 0.3 >"$TMP/store3.out"
expect "a deleted room stays empty after a restart" "$TMP/store3.out" "HISTORY #gone 0"
stop_server store3

# 22. typed agent messaging
PORT=$((BASE_PORT + 5))
start_server agents "$PORT" -T 1
client -u echo1 -G '{"skills":["echo"]}' -E -w 4 >"$TMP/echo1.out" &
ECHO1=$!
client -u streamer -G '{"skills":["stream"]}' -E -S -w 4 >"$TMP/streamer.out" &
STREAMER=$!
client -u silent -G '{"skills":["nothing"]}' -w 4 >"$TMP/silent.out" &
SILENT=$!
client -u plainuser -w 4 >/dev/null &
PLAIN=$!
wait_line "$TMP/echo1.out" "SYSTEM Welcome, echo1!"
wait_line "$TMP/streamer.out" "SYSTEM Welcome, streamer!"
wait_line "$TMP/silent.out" "SYSTEM Welcome, silent!"
sleep 0.2
client -u asker -Q -q "echo1:hello agent" -q "streamer:go" -q "plainuser:hi" -q "ghost:hi" -w 0.6 >"$TMP/asker.out"
expect "agent list shows advertised agents" "$TMP/asker.out" 'AGENTINFO echo1 1 {"skills":["echo"]}'
expect_not "agent list leaves out plain users" "$TMP/asker.out" "AGENTINFO plainuser"
expect "agent receives the request" "$TMP/echo1.out" "AREQ asker 1 hello agent"
expect "requester receives the response" "$TMP/asker.out" "ARESP echo1 1 0 1 echo: hello agent"
expect "streamed partial response" "$TMP/asker.out" "ARESP streamer 2 0 0 working"
expect "streamed final response" "$TMP/asker.out" "ARESP streamer 2 0 1 echo: go"
expect "request to a non-agent is refused" "$TMP/asker.out" "ERROR 10 "
expect "request to an unknown agent is refused" "$TMP/asker.out" "ERROR 6 No such agent"

raw >"$TMP/agentraw.out" <<'PY'
import sys, struct, time
from tundra_raw import *
port = int(sys.argv[1])
def req(agent, rid, body):
    return frame(0x21, s8(agent) + struct.pack(">Q", rid) + s16(body))
def resp(to, rid, status, final, body):
    return frame(0x22, s8(to) + struct.pack(">QBB", rid, status, final) + s16(body))

a = Conn(port)
a.send(register(b"rawasker"))
a.wait_for(0x10)
a.send(req(b"silent", 7, b"first"))
a.send(req(b"silent", 7, b"again"))
f = a.wait_for(0x15)
print("duplicate-id", error_code(f[1]) if f else None)

m = Conn(port)
m.send(register(b"meddler"))
m.wait_for(0x10)
m.send(resp(b"rawasker", 7, 0, 1, b"spoofed"))
f = m.wait_for(0x15)
print("spoofed-response", error_code(f[1]) if f else None)

f = a.wait_for(0x32, 4)
if f:
    p = f[1]
    nl = p[0]; rid, status, final = struct.unpack(">QBB", p[1 + nl:11 + nl])
    print("timeout", p[1:1 + nl].decode(), rid, status, final)
else:
    print("timeout none")

a.send(frame(0x20, s16(b"x" * 8193)))
f = a.wait_for(0x15)
print("caps-too-large", error_code(f[1]) if f else None)
PY
expect "duplicate request id refused" "$TMP/agentraw.out" "duplicate-id 11"
expect "response to someone else's request refused" "$TMP/agentraw.out" "spoofed-response 11"
expect "unanswered request times out" "$TMP/agentraw.out" "timeout silent 7 3 1"
expect "oversized capabilities refused" "$TMP/agentraw.out" "caps-too-large 2"
wait $ECHO1 $STREAMER $SILENT $PLAIN
stop_server agents

# 23. requests to an agent that goes away are closed
PORT=$((BASE_PORT + 6))
start_server agents2 "$PORT" -g 1
client -u flaky -G '{}' -w 0.3 >"$TMP/flaky.out"
client -u waiter -q "flaky:are you there" -w 4 >"$TMP/waiter.out"
expect "request to a vanished agent gets unavailable" "$TMP/waiter.out" "ARESP flaky 1 4 1 "
stop_server agents2

# 23b. agent edge cases from review
PORT=$((BASE_PORT + 9))
start_server agents3 "$PORT" -T 2
raw >"$TMP/agentedge.out" <<'PY'
import sys, struct, time
from tundra_raw import *
port = int(sys.argv[1])

def req(agent, rid, body):
    return frame(0x21, s8(agent) + struct.pack(">Q", rid) + s16(body))
def resp(to, handle, status, final, body):
    return frame(0x22, s8(to) + struct.pack(">QBB", handle, status, final) + s16(body))
def parse_req_in(p):
    n = p[0]; who = p[1:1 + n]; handle = struct.unpack(">Q", p[1 + n:9 + n])[0]
    return who.decode(), handle, p[11 + n:].decode()
def parse_resp_in(p):
    n = p[0]; rid, status, final = struct.unpack(">QBB", p[1 + n:11 + n])
    return rid, status, final, p[13 + n:].decode()

# 1. a late answer to a timed-out request must not answer a newer request with the same id
ag = Conn(port)
ag.send(register(b"lateagent"))
sid = ag.wait_for(0x10)[1][:16]
ag.send(frame(0x20, s16(b"{}")))
time.sleep(0.2)
ag.sock.close()
rq = Conn(port, timeout=4)
rq.send(register(b"latereq"))
rq.wait_for(0x10)
rq.send(req(b"lateagent", 5, b"first"))
f = rq.wait_for(0x32, 4)
print("first-timeout", parse_resp_in(f[1])[:3] if f else None)
rq.send(req(b"lateagent", 5, b"second"))
ag = Conn(port)
ag.send(register(b"lateagent", sid))
ag.wait_for(0x10)
got = {}
for _ in range(2):
    f = ag.wait_for(0x31)
    if f:
        who, handle, body = parse_req_in(f[1])
        got[body] = handle
print("agent-sees-distinct-handles", len(got) == 2 and got.get("first") != got.get("second"))
ag.send(resp(b"latereq", got.get("first", 0), 0, 1, b"late answer"))
f = ag.wait_for(0x15)
print("late-answer-refused", error_code(f[1]) if f else None)
ag.send(resp(b"latereq", got.get("second", 0), 0, 1, b"right answer"))
f = rq.wait_for(0x32)
print("right-answer", parse_resp_in(f[1]) if f else None)

# 2. requests dropped from a detached agent's full backlog fail at once
ag2 = Conn(port)
ag2.send(register(b"busyagent"))
ag2.wait_for(0x10)
ag2.send(frame(0x20, s16(b"{}")))
time.sleep(0.2)
ag2.sock.close()
time.sleep(0.2)
askers = []
for i in range(2):
    a = Conn(port)
    a.send(register(b"asker%d" % i))
    a.wait_for(0x10)
    askers.append(a)
start = time.time()
for i in range(30):
    for a in askers:
        a.send(req(b"busyagent", i, b"x"))
# count only what arrives in the first 1.2 s, well before the 2 s timeout
unavailable = 0
while time.time() - start < 1.2:
    for a in askers:
        a.sock.settimeout(0.05)
        f = a.recv_frame()
        if f and f[0] == 0x32 and parse_resp_in(f[1])[1] == 4:
            unavailable += 1
print("dropped-requests-fail-fast", unavailable >= 10, unavailable)

# 3. turning off advertising closes open requests
ag3 = Conn(port)
ag3.send(register(b"quitter"))
ag3.wait_for(0x10)
ag3.send(frame(0x20, s16(b"{}")))
r3 = Conn(port)
r3.send(register(b"quitreq"))
r3.wait_for(0x10)
time.sleep(0.1)
r3.send(req(b"quitter", 1, b"anyone?"))
ag3.wait_for(0x31)
ag3.send(frame(0x20, s16(b"")))
f = r3.wait_for(0x32)
print("unadvertise-closes", parse_resp_in(f[1])[:3] if f else None)

# 4. an agent list that does not fit is marked truncated
for i in range(10):
    c = Conn(port)
    c.send(register(b"bigcaps%d" % i))
    c.wait_for(0x10)
    c.send(frame(0x20, s16(b"c" * 8192)))
    c.sock.close()
time.sleep(0.2)
l = Conn(port)
l.send(register(b"lister"))
l.wait_for(0x10)
l.send(frame(0x23))
f = l.wait_for(0x30)
print("list-truncated", f[1][0] if f else None, struct.unpack(">H", f[1][1:3])[0] if f else None)
PY
expect "late answer: first request times out" "$TMP/agentedge.out" "first-timeout (5, 3, 1)"
expect "late answer: agent sees distinct handles" "$TMP/agentedge.out" "agent-sees-distinct-handles True"
expect "late answer to a closed request is refused" "$TMP/agentedge.out" "late-answer-refused 11"
expect "answer to the new request reaches the requester" "$TMP/agentedge.out" "right-answer (5, 0, 1, 'right answer')"
expect "requests dropped from a full backlog fail fast" "$TMP/agentedge.out" "dropped-requests-fail-fast True"
expect "turning off advertising closes open requests" "$TMP/agentedge.out" "unadvertise-closes (1, 4, 1)"
expect "an oversized agent list is marked truncated" "$TMP/agentedge.out" "list-truncated 1"
stop_server agents3
# 24. WebSocket gateway and web client
PORT=$((BASE_PORT + 7))
WSPORT=$((BASE_PORT + 8))
start_server ws "$PORT" -w "$WSPORT"
raw >"$TMP/ws.out" <<'PY'
import sys, struct, socket, urllib.request, urllib.error
from tundra_raw import *
port = int(sys.argv[1]); wsport = port + 1

w = WsConn(wsport)
print("accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in w.handshake)
w.send(register(b"webuser"))
f = w.recv_frame()
print("ws-register", f[0] if f else None)

t = Conn(port)
t.send(register(b"tcpuser"))
t.wait_for(0x10)
t.send(frame(0x02, s16(b"tcp to ws")))
f = w.wait_for(0x17)
print("tcp-to-ws", f[1][-9:].decode() if f else None)
w.send(frame(0x02, s16(b"ws to tcp")))
f = t.wait_for(0x17)
print("ws-to-tcp", f[1][-9:].decode() if f else None)

big = b"z" * 60000
w.send(frame(0x02, s16(big)))
f = t.wait_for(0x17)
print("ws-large", len(f[1]) if f else None)
t.send(frame(0x02, s16(big)))
f = w.wait_for(0x17)
print("tcp-large-to-ws", len(f[1]) if f else None)

w.sock.sendall(WsConn.ws_frame(b"hi", opcode=0x9))
m = w.recv_ws()
print("ping-pong", m)

w.sock.sendall(WsConn.ws_frame(b"text", opcode=0x1))
m = w.recv_ws()
print("text-refused", m[0] if m else None, struct.unpack(">H", m[1][:2])[0] if m else None)

u = WsConn(wsport)
u.send(register(b"unmasked"))
u.recv_frame()
u.sock.sendall(bytes([0x82, 0x02]) + b"ab")
m = u.recv_ws()
while m and m[0] != 8:  # skip frames queued before the close
    m = u.recv_ws()
print("unmasked-refused", struct.unpack(">H", m[1][:2])[0] if m else None)

body = urllib.request.urlopen("http://127.0.0.1:%d/" % wsport).read()
print("web-client", b"<title>Tundra</title>" in body)
try:
    urllib.request.urlopen("http://127.0.0.1:%d/nope" % wsport)
except urllib.error.HTTPError as e:
    print("http-404", e.code)
bad = WsConn(wsport, key="")
print("bad-handshake", bad.handshake.split("\r\n")[0])

# upgrade request and a large first frame in one write is not a 431
sock = socket.create_connection(("127.0.0.1", wsport))
sock.settimeout(2)
req = ("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").encode()
reg = WsConn.ws_frame(register(b"eager"))
pad = WsConn.ws_frame(frame(0x02, s16(b"p" * 9000)))  # a 9 KB frame before REGISTER: gets an error, then registers
sock.sendall(req + pad + reg)
resp = b""
while b"\r\n\r\n" not in resp:
    resp += sock.recv(4096)
e = WsConn.__new__(WsConn); e.sock = sock; e.raw = resp.partition(b"\r\n\r\n")[2]; e.buf = b""
f = e.wait_for(0x10)
print("handshake-plus-frames", resp.split(b"\r\n")[0].decode(), f[0] if f else None)

# a fragmented ping is a protocol error; a close code is echoed
g = WsConn(wsport); g.send(register(b"fragger")); g.recv_frame()
g.sock.sendall(WsConn.ws_frame(b"x", opcode=0x9, fin=False))
m = g.recv_ws()
while m and m[0] != 8:
    m = g.recv_ws()
print("fragmented-control", struct.unpack(">H", m[1][:2])[0] if m else None)
h = WsConn(wsport); h.send(register(b"closer")); h.recv_frame()
h.sock.sendall(WsConn.ws_frame(struct.pack(">H", 4001), opcode=0x8))
m = h.recv_ws()
while m and m[0] != 8:
    m = h.recv_ws()
print("close-echo", struct.unpack(">H", m[1][:2])[0] if m else None)
PY
expect "websocket handshake accept key" "$TMP/ws.out" "accept True"
expect "register over websocket" "$TMP/ws.out" "ws-register 16"
expect "chat from tcp reaches websocket" "$TMP/ws.out" "tcp-to-ws tcp to ws"
expect "chat from websocket reaches tcp" "$TMP/ws.out" "ws-to-tcp ws to tcp"
# CHAT payload: id 8 + ts 8 + kind 1 + "#lobby" 7 + sender 8 + len 2 + text 60000
expect "large message from websocket" "$TMP/ws.out" "ws-large 60034"
expect "large message to websocket" "$TMP/ws.out" "tcp-large-to-ws 60034"
expect "websocket ping answered" "$TMP/ws.out" "ping-pong (10, b'hi')"
expect "text messages refused with 1003" "$TMP/ws.out" "text-refused 8 1003"
expect "unmasked client frames refused with 1002" "$TMP/ws.out" "unmasked-refused 1002"
expect "web client served at /" "$TMP/ws.out" "web-client True"
expect "unknown paths 404" "$TMP/ws.out" "http-404 404"
expect "bad handshake is a 400" "$TMP/ws.out" "bad-handshake HTTP/1.1 400 Bad Request"
expect "frames sent with the upgrade request are fine" "$TMP/ws.out" "handshake-plus-frames HTTP/1.1 101 Switching Protocols 16"
expect "fragmented control frames are refused with 1002" "$TMP/ws.out" "fragmented-control 1002"
expect "a close code is echoed" "$TMP/ws.out" "close-echo 4001"

# 25. Python SDK
if python3 "$ROOT/tests/sdk_test.py" "$PORT" >"$TMP/sdk.out" 2>&1; then :; else FAILED=1; fi
cat "$TMP/sdk.out"
stop_server ws

if [ "$FAILED" -ne 0 ]; then
    for log in "$TMP"/*.log; do echo "--- $(basename "$log") ---"; tail -40 "$log"; done
    exit 1
fi
echo "all tests passed"
