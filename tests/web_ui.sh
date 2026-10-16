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
(cd "$ROOT/tests/web" && node ui_test.js "$WSPORT")
STATUS=$?
kill "$AGENT" "$SERVER" 2>/dev/null
wait 2>/dev/null
rm -f "$LOG"
exit $STATUS
