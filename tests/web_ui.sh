#!/usr/bin/env bash
# Runs the web client in jsdom against a fresh server. Needs node and
# `npm install` in tests/web (CI does this).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${TUNDRA_TEST_PORT:-$((20000 + RANDOM % 10000))}"
WSPORT=$((PORT + 1))
LOG="$(mktemp)"
"$ROOT/build/tundra_chat_server" -p "$PORT" -w "$WSPORT" 2>"$LOG" &
SERVER=$!
for _ in $(seq 100); do grep -q "listening on port" "$LOG" && break; sleep 0.05; done
"$ROOT/build/tundra_client" -p "$PORT" -u webecho -G '{"skills":["echo"]}' -E -w 15 >/dev/null &
AGENT=$!
sleep 0.3
FLAG="$(mktemp -u)"
OUT="$(mktemp)"
(cd "$ROOT/tests/web" && node ui_test.js "$WSPORT" "$FLAG") >"$OUT" 2>&1 &
NODE=$!
# restart the server when the test asks for it
for _ in $(seq 300); do grep -q "restart-now" "$OUT" && break; sleep 0.1; done
kill "$SERVER" 2>/dev/null; wait "$SERVER" 2>/dev/null
"$ROOT/build/tundra_chat_server" -p "$PORT" -w "$WSPORT" 2>"$LOG" &
SERVER=$!
for _ in $(seq 100); do grep -q "listening on port" "$LOG" && break; sleep 0.05; done
touch "$FLAG"
wait "$NODE"
STATUS=$?
grep -v "restart-now" "$OUT"
rm -f "$OUT" "$FLAG"
kill "$AGENT" "$SERVER" 2>/dev/null
wait 2>/dev/null
rm -f "$LOG"
exit $STATUS
