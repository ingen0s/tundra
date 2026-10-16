# Tundra

A small, fast chat server in C with a compact binary protocol. Rooms, direct messages, history and session resume for people, and typed request/response routing for software agents, over the same wire.

## Status

Early but tested. The server is single-threaded on Linux epoll, handles up to 1,000 connections, and every change runs through unit tests, end-to-end tests, AddressSanitizer/UBSan and a fuzzer in CI. There is no built-in TLS yet, so on untrusted networks put it behind a TLS-terminating proxy.

## Features

- **Binary protocol:** 7-byte header, length-prefixed fields, strict validation. Full spec in [docs/PROTOCOL.md](docs/PROTOCOL.md).
- **Rooms:** join, leave, list, who's here. Everyone starts in `#lobby`.
- **Direct messages** between users.
- **Message ids and history:** every message gets a server-assigned id; the most recent 10,000 are kept in memory and can be paged with `HISTORY`.
- **Session resume with exact catch-up:** a dropped connection keeps its name and rooms for 2 minutes. Reconnecting replays every message it missed from the server's log, starting after the client's last `ACK`, an explicit resume point, or what the server knows it wrote, paced so a long catch-up never overflows the connection.
- **Agents:** any session can advertise capabilities; others send it typed requests with their own ids and get back streamed or single responses. The server enforces that answers match open requests and closes requests that time out or whose agent leaves.
- **Browsers and Python:** an optional HTTP port serves a bundled web client and a WebSocket endpoint that carries the same frames; `sdk/python` is a dependency-free asyncio client with agent support and automatic reconnect.
- **Optional token auth:** a `username token` file protects listed names (or, with `-A`, all of them); re-read on `SIGHUP`.
- **Optional persistence:** with `-d chat.db`, messages go to SQLite; history pages back indefinitely and ids continue across restarts.
- **Health:** ping/pong latency and per-session counters via `HEALTH_CHECK`.
- **Backpressure:** each client's outgoing queue is capped at 1 MiB; clients that can't keep up are disconnected instead of stalling the server.

## Quick start

```bash
sudo apt-get install uuid-dev libsqlite3-dev
make
./build/tundra_chat_server            # listens on 4242
./build/tundra_chat_server -w 8080    # also serves the web client at http://localhost:8080/
./build/tundra_chat_server -d chat.db -t tokens.txt   # with persistence and token auth
```

In two more terminals:

```bash
./build/tundra_client -u alice -j '#dev' -w 60
./build/tundra_client -u bob -j '#dev' -t '#dev' -m 'hello dev' -t alice -m 'hi alice (dm)'
```

An agent and a client that uses it:

```bash
./build/tundra_client -u echo -G '{"skills":["echo"]}' -E -w 60
./build/tundra_client -u carol -Q -q 'echo:summarize the standup'
```

## Test client

`tundra_client` registers, runs the actions you give it in order, then prints everything it receives for `-w` seconds (one line per frame) and answers pings.

| Option | Action |
|--------|--------|
| `-u name` | username (required) |
| `-H host`, `-p port` | server address (default `127.0.0.1:4242`) |
| `-r id` | resume a session with the id from a previous `SESSION` line |
| `-f id` | with `-r`: replay everything after this message id |
| `-a` | acknowledge every chat message received |
| `-k token` | authentication token for this username |
| `-G caps` | advertise as an agent with these capabilities |
| `-q agent:body` | send an agent request |
| `-Q` | list agents |
| `-E` / `-S` | act as an echo agent / stream a partial answer first |
| `-j #room` / `-L #room` | join / leave a room |
| `-t target` | target for following `-m`: a `#room` or a username (default `#lobby`) |
| `-m text` | send a message |
| `-R` | list rooms |
| `-W #room` | list a room's members |
| `-y #room` | fetch recent history |
| `-c` | health check |
| `-w seconds` | how long to listen afterwards (default 1) |

## Python SDK

`sdk/python/tundra` has no dependencies beyond the standard library:

```python
import asyncio, tundra

async def main():
    client = tundra.Client("localhost", 4242, "alice")
    await client.connect()
    await client.join("#dev")
    await client.send("#dev", "hello")
    print(await client.request("echo", "summarize this"))   # ask an agent
    async for msg in client.messages():
        print(msg.sender, msg.text)

asyncio.run(main())
```

Writing an agent is a handler; `yield` streams partial answers:

```python
agent = tundra.Agent("localhost", 4242, "summarizer", capabilities={"skills": ["summarize"]})

@agent.handler
async def handle(req):
    yield "working on it"
    yield "summary of: " + req.body

asyncio.run(agent.run_forever())
```

The client answers pings, acknowledges what it receives, and on a dropped connection reconnects and resumes its session, catching up on anything it missed. See `sdk/python/examples/`.

## Development

```bash
make test                               # codec unit tests + end-to-end tests
make asan && ./tests/integration.sh     # same, under AddressSanitizer + UBSan
make fuzz-run FUZZ_TIME=60              # fuzz the payload codec (needs clang)
(cd tests/web && npm install) && ./tests/web_ui.sh   # web client in jsdom (needs node)
```

Layout:

| Path | What |
|------|------|
| `tundra_protocol.h` | wire constants, big-endian helpers, bounds-checked reader/writer |
| `tundra_codec.{h,c}` | decode/encode for every message type |
| `server/` | server: `net.c` (epoll, I/O), `ws.c` (HTTP/WebSocket), `session.c`, `rooms.c`, `msglog.c`, `store.c` (SQLite), `auth.c`, `agents.c`, `handlers.c`, `main.c` |
| `web/index.html` | bundled browser client (embedded into the server at build time) |
| `sdk/python/` | Python SDK and examples |
| `test_client.c` | scriptable CLI client |
| `tests/` | codec unit tests and `integration.sh` |
| `fuzz/` | libFuzzer target and seed corpus |
| `docs/PROTOCOL.md` | protocol specification |

Adding a message type: define it in `tundra_protocol.h`, add decode/encode and a struct in `tundra_codec.{h,c}` (the fuzzer checks they round-trip), add a seed to `fuzz/corpus`, handle it in `server/handlers.c`, and cover it in `tests/integration.sh`.

## Configuration

Server options:

| Option | Meaning |
|--------|---------|
| `-p port` | listen port (default 4242) |
| `-w port` | also serve the web client and WebSockets on this HTTP port |
| `-d file` | store messages in this SQLite database |
| `-t file` | tokens file, one `username token` per line; keep it private (`chmod 600`) |
| `-A` | with `-t`: every username must be listed |
| `-i seconds` | idle timeout (default 120) |
| `-g seconds` | how long a disconnected session is held (default 120) |
| `-T seconds` | how long an agent request may stay unanswered (default 60) |
| `-v` | log every frame |

Compile-time constants in `server/server.h` (two have command-line overrides):

| Constant | Default | Meaning |
|----------|---------|---------|
| `MAX_CLIENTS` | 1000 | concurrent connections |
| `MAX_ROOMS` | 256 | rooms |
| `DEFAULT_IDLE_TIMEOUT` | 120 s | drop connections that send nothing (`-i`) |
| `DEFAULT_RESUME_GRACE` | 120 s | how long a disconnected session is held (`-g`) |
| `REGISTER_TIMEOUT` | 10 s | time a new connection has to register |
| `PING_INTERVAL` | 30 s | server pings |
| `MAX_BUFFERED_MESSAGES` / `MAX_BACKLOG_BYTES` | 50 / 1 MiB | buffered non-chat frames per disconnected session |
| `MAX_TOTAL_BACKLOG` | 64 MiB | all backlogs together |
| `MAX_OUTBOUND_BYTES` | 1 MiB | outgoing queue per client |
| `MSGLOG_CAPACITY` / `MSGLOG_MAX_BYTES` | 10,000 / 64 MiB | history kept in memory |

## Roadmap

- Built-in TLS (today: put a TLS-terminating proxy in front)
- Persisting sessions and room membership across restarts (messages already persist with `-d`)
- SDKs for more languages

## License

MIT, see [LICENSE](LICENSE).
