# Tundra

A small, fast chat server in C with a compact binary protocol. Rooms, direct messages, history and session resume, built for both people and software agents talking over the same wire.

## Status

Early but tested. The server is single-threaded on Linux epoll, handles up to 1,000 connections, and every change runs through unit tests, end-to-end tests, AddressSanitizer/UBSan and a fuzzer in CI. There is no TLS or authentication yet, so run it on a trusted network or behind a TLS-terminating proxy.

## Features

- **Binary protocol:** 7-byte header, length-prefixed fields, strict validation. Full spec in [docs/PROTOCOL.md](docs/PROTOCOL.md).
- **Rooms:** join, leave, list, who's here. Everyone starts in `#lobby`.
- **Direct messages** between users.
- **Message ids and history:** every message gets a server-assigned id; the most recent 10,000 are kept in memory and can be paged with `HISTORY`.
- **Session resume:** a dropped connection keeps its name and rooms for 2 minutes; reconnecting with the session id delivers what was missed (up to 50 messages).
- **Health:** ping/pong latency and per-session counters via `HEALTH_CHECK`.
- **Backpressure:** each client's outgoing queue is capped at 1 MiB; clients that can't keep up are disconnected instead of stalling the server.

## Quick start

```bash
sudo apt-get install uuid-dev
make
./build/tundra_chat_server            # listens on 4242; -p PORT, -v for frame logging
```

In two more terminals:

```bash
./build/tundra_client -u alice -j '#dev' -w 60
./build/tundra_client -u bob -j '#dev' -t '#dev' -m 'hello dev' -t alice -m 'hi alice (dm)'
```

## Test client

`tundra_client` registers, runs the actions you give it in order, then prints everything it receives for `-w` seconds (one line per frame) and answers pings.

| Option | Action |
|--------|--------|
| `-u name` | username (required) |
| `-H host`, `-p port` | server address (default `127.0.0.1:4242`) |
| `-r id` | resume a session with the id from a previous `SESSION` line |
| `-j #room` / `-L #room` | join / leave a room |
| `-t target` | target for following `-m`: a `#room` or a username (default `#lobby`) |
| `-m text` | send a message |
| `-R` | list rooms |
| `-W #room` | list a room's members |
| `-y #room` | fetch recent history |
| `-c` | health check |
| `-w seconds` | how long to listen afterwards (default 1) |

## Development

```bash
make test                               # codec unit tests + end-to-end tests
make asan && ./tests/integration.sh     # same, under AddressSanitizer + UBSan
make fuzz-run FUZZ_TIME=60              # fuzz the payload codec (needs clang)
```

Layout:

| Path | What |
|------|------|
| `tundra_protocol.h` | wire constants, big-endian helpers, bounds-checked reader/writer |
| `tundra_codec.{h,c}` | decode/encode for every message type |
| `server/` | server: `net.c` (epoll, I/O), `session.c`, `rooms.c`, `msglog.c`, `handlers.c`, `main.c` |
| `test_client.c` | scriptable CLI client |
| `tests/` | codec unit tests and `integration.sh` |
| `fuzz/` | libFuzzer target and seed corpus |
| `docs/PROTOCOL.md` | protocol specification |

Adding a message type: define it in `tundra_protocol.h`, add decode/encode and a struct in `tundra_codec.{h,c}` (the fuzzer checks they round-trip), add a seed to `fuzz/corpus`, handle it in `server/handlers.c`, and cover it in `tests/integration.sh`.

## Configuration

Compile-time constants in `server/server.h`:

| Constant | Default | Meaning |
|----------|---------|---------|
| `MAX_CLIENTS` | 1000 | concurrent connections |
| `MAX_ROOMS` | 256 | rooms |
| `IDLE_TIMEOUT` | 120 s | drop connections that send nothing |
| `RESUME_GRACE` | 120 s | how long a disconnected session is held |
| `PING_INTERVAL` | 30 s | server pings |
| `MAX_BUFFERED_MESSAGES` | 50 | backlog per disconnected session |
| `MAX_OUTBOUND_BYTES` | 1 MiB | outgoing queue per client |
| `MSGLOG_CAPACITY` / `MSGLOG_MAX_BYTES` | 10,000 / 64 MiB | history kept in memory |

## Roadmap

- Delivery ACKs and resume-from-last-seen-id (exact catch-up instead of a fixed backlog)
- Token authentication and persistent history (SQLite)
- Typed agent messaging: agents advertise capabilities, request/response with correlation ids
- WebSocket gateway, browser client, Python SDK

## License

MIT, see [LICENSE](LICENSE).
