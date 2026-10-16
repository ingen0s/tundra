# @tundra/client

TypeScript SDK for the [Tundra](../../README.md) chat server. One package for Node and browsers, no runtime dependencies.

- `Client`: rooms, direct messages, history, room lists, agent requests (single or streamed).
- `Agent`: advertises capabilities and answers requests with a handler that returns or yields.
- Answers pings, acknowledges what it receives (batched), and on a dropped connection reconnects and resumes its session, catching up on anything missed. Messages repeated by the catch-up are dropped by id.
- Transports: TCP (and TLS) in Node, WebSocket in browsers and Node 22+.
- A bounds-checked codec for every message type, exported for tools and tests.

Message ids, timestamps inside the codec and request handles are `bigint`, since they are 64-bit on the wire.

## Build

```bash
cd sdk/typescript
npm install
npm run build        # tsc, output in dist/
```

## Node

```ts
import { Client } from "@tundra/client";

const client = await Client.connect({ host: "localhost", port: 4242, username: "alice" });
await client.join("#dev");
const id = await client.send("#dev", "hello");          // bigint message id
await client.send("bob", "a direct message");

console.log(await client.history("#dev", { limit: 20 }));
console.log(await client.rooms());                       // { "#lobby": 3, "#dev": 2 }
console.log(await client.request("echo", "summarize this"));

for await (const msg of client.messages()) {
  console.log(msg.room ?? "(dm)", msg.sender, msg.text);
}
```

TLS (for a server behind a TLS-terminating proxy): pass `tls: true`, or `node:tls` options such as `tls: { ca, servername }`.

WebSocket from Node 22+ works the same way with `url` instead of `host`/`port`:

```ts
const client = await Client.connect({ url: "ws://localhost:8080/ws", username: "alice" });
```

## Browsers

Start the server with `-w 8080` and connect to its `/ws` endpoint:

```ts
import { Client } from "@tundra/client";

const client = new Client({ url: `wss://${location.host}/ws`, username: "alice", token });
client.on("chat", (msg) => render(msg));
client.on("roomEvent", (ev) => console.log(ev.user, ev.event, ev.room));
client.on("disconnected", () => showStatus("reconnecting..."));
client.on("closed", ({ error }) => error && showLogin(error.text));
await client.connect();
```

The TCP transport lives in `dist/tcp.js`, loaded with a dynamic import only when a TCP client connects, and is marked `false` in the package's `browser` field, so bundlers leave `node:net` and `node:tls` out. Browser code type-checks without `@types/node`.

## Agents

```ts
import { Agent, AgentError } from "@tundra/client";

const agent = new Agent({
  host: "localhost",
  port: 4242,
  username: "summarizer",
  capabilities: { skills: ["summarize"] },    // sent as JSON; a string is sent as is
});

agent.handler(async function* (req) {
  yield "working on it";                       // partial answer
  if (!req.body) throw new AgentError("summarizer", 2, "empty request");  // status REJECTED
  yield "summary of: " + req.body;             // the last one is final
});

await agent.runForever();
```

A handler may also return a value (or a promise of one). Throwing answers with status `ERROR` and the error message; throwing an `AgentError` answers with its status and body. `req.signal` is aborted when the agent closes or its session is replaced.

Callers:

```ts
const answer = await client.request("summarizer", "the standup notes");
for await (const part of client.requestStream("summarizer", "the standup notes")) console.log(part);
const agents = await client.agents();         // { summarizer: { online: true, capabilities: { skills: [...] } } }
```

`request` and `requestStream` throw `AgentError` for a non-ok status (including the server's `TIMEOUT` and `UNAVAILABLE`), `TundraError` when the server refuses the request (`NO_SUCH_TARGET`, `NOT_AN_AGENT`, `BAD_REQUEST`), and `RequestTimeoutError` when nothing arrives within `timeoutMs` (default 120 s).

## Reconnects and errors

- `connect()` rejects with a `TundraError` when registration is refused (name taken, invalid name, bad token). A refused client is left closed, with no background retries.
- After a drop the client retries with backoff (200 ms doubling to 5 s), resumes its session and sends `RESUME_FROM` with the newest id it has, so nothing is missed. Pending calls fail with `ConnectionError`; agent answers still arrive after a resume.
- If the session expired, it registers a fresh one at once, forgets the ids it has seen (a server without a database may restart ids), rejoins its rooms, and agents re-advertise.
- Errors that retrying cannot fix (`USERNAME_TAKEN`, `INVALID_NAME`, `UNAUTHORIZED`) stop the client: `client.error` is set, `closed` fires with the error, and `messages()` throws it.
- Set `reconnect: false` to close instead of reconnecting.

## API

`new Client(options)` / `await Client.connect(options)`

| Option | Default | Meaning |
|--------|---------|---------|
| `username` | required | |
| `token` | | authentication token |
| `host`, `port` | `127.0.0.1`, `4242` | TCP server (Node) |
| `tls` | | `true` or `node:tls` options |
| `url` | | WebSocket URL; selects the WebSocket transport |
| `WebSocket` | global | WebSocket constructor for runtimes without one |
| `transport` | | a custom `TransportFactory` |
| `reconnect` | `true` | reconnect and resume after a drop |
| `callTimeoutMs` | `10000` | wait for replies to `send`, `history`, ... |
| `ackDelayMs` | `200` | ACK batching window |
| `maxBufferedMessages` | `10000` | unread messages kept for `messages()` |
| `reconnectDelayMs`, `maxReconnectDelayMs` | `200`, `5000` | backoff |

Methods: `connect()`, `close()`, `ready()`, `join(room)`, `leave(room)`, `send(target, text)`, `history(room, { limit, beforeId })`, `rooms()`, `who(room)`, `health()`, `agents()`, `request(agent, body, { timeoutMs })`, `requestStream(...)`, `messages()`, `nextMessage(timeoutMs?)`.

Properties: `state`, `connected`, `sessionId`, `lastId`, `joinedRooms`, `agentsTruncated`, `error`.

Events (`client.on(name, fn)` returns an unsubscribe function; also `once`, `off`, and `next(name)` for a promise): `connected`, `disconnected`, `reconnecting`, `closed`, `chat`, `roomEvent`, `system`, `error`, `broadcast`, `frame`.

Each chat message is delivered once to `messages()` / `nextMessage()` (they share one buffer) and to every `chat` listener.

## Tests

`test/sdk.test.mjs` runs with plain `node --test` against a live server. From the repository root, `./tests/ts_sdk.sh` builds the server, starts it and runs everything. To run against your own server:

```bash
TUNDRA_PORT=4242 TUNDRA_WS_PORT=8080 npm test
```
