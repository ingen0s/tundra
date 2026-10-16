# Tundra Protocol, version 1

`tundra_protocol.h` and `tundra_codec.c` are the reference implementation; this document mirrors them.

## Framing

Every frame is a 7-byte header followed by `Length` payload bytes. All integers are big-endian.

```
offset 0  version  u8    0x01
offset 1  flags    u8    reserved: send 0, ignored by the server
offset 2  seq      u16   sequence number
offset 4  length   u16   payload bytes after the header
offset 6  type     u8    message type
offset 7  payload  length bytes
```

A frame is always exactly `7 + length` bytes, so the largest frame is 65542 bytes.

**Sequence numbers.** Clients number their own requests. Replies (`SYSTEM` welcome, `SENT`, `ERROR`, `HEALTH_RESULT`, `ROOM_LIST`, `WHO_RESULT`, `HISTORY_RESULT`) echo the sequence number of the request they answer. Everything the server pushes on its own (chat, room events, pings) uses a per-session counter.

**Strictness.** Every payload must be exactly the size its fields describe. Missing or trailing bytes get `ERR_INVALID_FORMAT`. A frame with an unknown version gets an error and the connection is closed.

## Transports

- **TCP** (default port 4242): frames are written back to back on the stream.
- **WebSocket** (`-w PORT`, path `/ws`): each binary WebSocket message carries exactly one Tundra frame, in both directions. Client messages must be unfragmented binary messages (what browsers send) holding exactly one complete frame. The server closes the connection with 1003 for text or continuation frames, 1002 for unmasked frames, extension bits, fragmented or oversized control frames and malformed close frames, 1007 for a binary message that is fragmented or is not exactly one frame, and 1009 for messages larger than one frame. A close from the client is answered with the same code. HTTP request headers are limited to 8 KiB. WebSocket ping/pong is answered, but the Tundra `PING`/`PONG` still applies. The same port serves the bundled web client at `/`.

## Field notation

| Notation | Meaning |
|----------|---------|
| `u8`, `u16`, `u32`, `u64` | unsigned big-endian integers |
| `str8` | `u8` length, then that many bytes |
| `str16` | `u16` length, then that many bytes |
| `id16` | 16 raw bytes (a UUID) |

Text is UTF-8 by convention; the server does not inspect it. Chat text is limited to 65,413 bytes so that it fits in every frame that carries it, including a `HISTORY_RESULT` entry with maximum-length names.

## Names

- **Usernames:** 1 to 32 printable ASCII characters, no spaces, must not start with `#`.
- **Rooms:** `#` followed by 1 to 31 of `a-z 0-9 _ -`.

## Client to server

Client message types are 0x01-0x0F and 0x20-0x2F; server message types are 0x10-0x1F and 0x30-0x3F.

| Type | Code | Payload |
|------|------|---------|
| `REGISTER` | 0x01 | `id16 id` `str8 username`, then optional extensions (see below). Must be sent within 10 seconds of connecting. |
| `MESSAGE` | 0x02 | `str16 text`. Shorthand for `SEND` to `#lobby`. |
| `AGENT_COMMAND` | 0x03 | `str8 agent` `str16 command`. Legacy; broadcast to `#lobby` as `BROADCAST_AGENT`. |
| `PONG` | 0x04 | echo of the `PING` payload. Required: it keeps the connection alive and latency is computed from the echoed timestamp. |
| `HEALTH_CHECK` | 0x05 | empty |
| `JOIN` | 0x06 | `str8 room`. Creates the room if needed. |
| `LEAVE` | 0x07 | `str8 room` |
| `SEND` | 0x08 | `u8 kind` `str8 target` `str16 text`. Kind 0 = room (target is a room), 1 = direct (target is a username). |
| `LIST_ROOMS` | 0x09 | empty |
| `ACK` | 0x0A | `u64 id`: every `CHAT` up to and including `id` has been processed. Cumulative; optional. |
| `HISTORY` | 0x0B | `str8 room` `u64 before_id` `u16 limit`. `before_id` 0 means "latest". `limit` 0 or over 100 means 100. |
| `WHO` | 0x0C | `str8 room` |
| `AGENT_ADVERTISE` | 0x20 | `str16 capabilities` (opaque, usually JSON, up to 8 KiB). Empty stops being an agent. |
| `AGENT_REQUEST` | 0x21 | `str8 agent` `u64 req_id` `str16 body`. `req_id` is chosen by the requester. |
| `AGENT_RESPONSE` | 0x22 | `str8 requester` `u64 handle` `u8 status` `u8 final` `str16 body`. `handle` is the one from `AGENT_REQUEST_IN`. |
| `LIST_AGENTS` | 0x23 | empty |

## Server to client

| Type | Code | Payload |
|------|------|---------|
| `SYSTEM` | 0x10 | `id16 id` `str16 text`. `id` is your session id in the welcome, zero otherwise. |
| `BROADCAST_AGENT` | 0x12 | `str8 sender` `u64 ts_ms` `str16 text` (text is `"<agent> <command>"`) |
| `PING` | 0x14 | `u64 ts_ms` |
| `ERROR` | 0x15 | `u16 code` `str16 text` |
| `HEALTH_RESULT` | 0x16 | `u64 ts_ms` `u8 status` `u32 latency_ms` `u32 uptime_ms` `u32 sent` `u32 received` |
| `CHAT` | 0x17 | `u64 id` `u64 ts_ms` `u8 kind` `str8 target` `str8 sender` `str16 text` |
| `SENT` | 0x18 | `u64 id`: the id assigned to your `SEND`/`MESSAGE` |
| `ROOM_LIST` | 0x19 | `u16 count`, then `count` x (`str8 room` `u16 members`) |
| `ROOM_EVENT` | 0x1A | `u8 event` `str8 room` `str8 user`. Event 1 = joined, 2 = left. |
| `HISTORY_RESULT` | 0x1B | `str8 room` `u16 count`, then `count` x (`u16 len` + a `CHAT` payload), oldest first |
| `WHO_RESULT` | 0x1C | `str8 room` `u16 count`, then `count` x (`str8 user` `u8 online`) |
| `AGENT_LIST` | 0x30 | `u8 truncated` `u16 count`, then `count` x (`str8 name` `u8 online` `str16 capabilities`). `truncated` is 1 when more agents exist than fit in one frame. |
| `AGENT_REQUEST_IN` | 0x31 | `str8 requester` `u64 handle` `str16 body`. `handle` is assigned by the server. |
| `AGENT_RESPONSE_IN` | 0x32 | `str8 agent` `u64 req_id` `u8 status` `u8 final` `str16 body` |

`BROADCAST_MESSAGE` (0x11) and `IMAGE` (0x13) are reserved and not sent.

Message ids are assigned by the server, are unique, and increase by one per message across all rooms and direct messages.

## REGISTER extensions

After the username, `REGISTER` may carry any number of extensions, each `u8 tag` `u16 len` `len bytes`. Unknown tags are ignored, so clients can send newer extensions to older servers.

| Tag | Name | Value |
|-----|------|-------|
| 0x01 | `RESUME_FROM` | `u64`: the newest `CHAT` id the client already has. On resume, everything after it is replayed. |
| 0x02 | `TOKEN` | bytes: authentication token for this username (see Authentication) |

## Authentication

Authentication is optional and configured on the server with a tokens file of `username token` lines (`-t`). A username listed there can only register with its token in the `TOKEN` extension; a missing or wrong token gets `UNAUTHORIZED` and the connection is closed. Names not in the file are open unless the server runs with `-A`, which requires every username to be listed. Resuming a session needs only its session id, which is itself a 122-bit secret. Tokens travel in clear text, so use TLS (for now, a TLS-terminating proxy) on untrusted networks.

The file is re-read on `SIGHUP` (Linux and macOS; Windows has no SIGHUP, so restart the server there). Loading fails closed: a file with any malformed line, a duplicate name, a token over 256 characters, or no entries at all is rejected as a whole; at startup the server refuses to start, and on reload the previous tokens stay in force. After a successful reload, sessions whose name no longer accepts the token they registered with are ended (`UNAUTHORIZED`, then disconnected).

## Sessions

1. `REGISTER` with an all-zero id. The welcome `SYSTEM` frame carries your session id, and you are joined to `#lobby` (you get a `ROOM_EVENT` for it).
2. If the connection drops, the session, its username and its room memberships are held for 2 minutes.
3. `REGISTER` with the same id and username to resume. The welcome comes first, then any `SYSTEM` notices about dropped frames or messages no longer available, then the catch-up described below. If the old connection is still open it is closed. An unknown or expired id, or someone else's id, gets `SESSION_EXPIRED`: register again with a zero id.
4. After the hold period the session is released, its username becomes free, and each of its rooms gets a "left" event. When the session table is full, the longest-detached session is released early to make room.

### Catching up after a resume

Chat messages are never buffered per session. Instead, a resumed session is caught up from the server's message log, in id order, starting after the first of these that applies:

1. the `RESUME_FROM` extension, if the client sent one;
2. the highest id the client has `ACK`ed, if it ever sent an `ACK`;
3. the highest id the server fully wrote to the previous connection.

Option 3 is a good default, but bytes written just before a connection drops can still be lost in transit, so a client that must not miss anything should either `ACK` or send `RESUME_FROM`. Replay can repeat messages the client already has (for example after `RESUME_FROM` with an old id); ids are unique and increasing, so clients should ignore any `CHAT` whose id they have already seen.

Only messages the session would have received are replayed: messages sent after it joined each room it is in now, and direct messages to it sent after the session was created; never its own, and never anything from before the session existed (whatever `RESUME_FROM` says). If the log no longer holds part of the range (it keeps 10,000 messages / 64 MiB), including when the log moves on during a slow catch-up, a `SYSTEM` notice says "Some earlier messages are no longer available" and replay continues from the oldest message still held. A `RESUME_FROM` value that is not 8 bytes is refused with `INVALID_FORMAT`.

Other frames (room events, notices) that arrive while a session is away, or while it is still catching up, are buffered: up to 50 frames and 1 MiB per session (64 MiB across the server), oldest dropped first, with a `SYSTEM` notice of how many were dropped. Each is delivered in its place among the replayed chats, right after the last message that came before it.

All catch-up is paced: it is sent only while the connection has less than 256 KiB queued, so a long catch-up never trips the 1 MiB output limit. New messages that arrive meanwhile are delivered after it, in order.

## Rooms

- `JOIN` creates a room if it does not exist. Members, including the joiner, get `ROOM_EVENT joined`.
- `LEAVE` sends `ROOM_EVENT left` to the remaining members and the leaver. Empty rooms are deleted, except `#lobby`.
- Room messages go to every member except the sender. Members who are disconnected get them in their backlog.
- `HISTORY` and sending require membership. `WHO` and `LIST_ROOMS` do not.
- A room that empties and is created again starts with an empty history.
- `JOIN` for a room you are already in gets `INVALID_FORMAT` ("Already in that room").
- The server keeps the most recent 10,000 messages (and at most 64 MiB of text) in memory. When it runs with a database (`-d`), room and direct messages are also written there, `HISTORY` reads from the database (so it can page back indefinitely), and the recent log survives restarts. Message ids are reserved in the database in blocks of 1,000 before use, so after a restart they keep increasing (possibly skipping some) and an id is never reused, even if an insert failed. A failed insert is logged; the message is still delivered.

## Agents

Agents are ordinary sessions that advertise capabilities. The server routes typed requests to them and their responses back, without interpreting either body.

1. An agent sends `AGENT_ADVERTISE`. `LIST_AGENTS` shows every advertised agent, whether it is online, and its capabilities.
2. A client sends `AGENT_REQUEST` with a request id of its choosing. The agent receives `AGENT_REQUEST_IN` naming the requester and carrying a server-assigned **handle**. Handles are never reused, so an answer to a request that has already been closed can never be mistaken for an answer to a newer one, even if the requester reuses its id. If the agent is briefly disconnected, the request waits in its backlog; if the backlog overflows and drops it, the requester gets `UNAVAILABLE` right away.
3. The agent answers with `AGENT_RESPONSE` giving the requester and the handle. It may send any number of partial responses (`final` = 0) followed by one final response (`final` = 1). The requester receives each as `AGENT_RESPONSE_IN` with its own request id.
4. The server only accepts a response whose handle belongs to an open request sent to that agent, so agents cannot answer requests they were not given or that have already closed.
5. An agent that sends an empty `AGENT_ADVERTISE` stops being an agent; its open requests are closed with `UNAVAILABLE`.

| Status | Name | Sent by |
|--------|------|---------|
| 0 | `OK` | agent |
| 1 | `ERROR` | agent |
| 2 | `REJECTED` | agent |
| 3 | `TIMEOUT` | server, when no response arrives for 60 seconds (`-T`); each partial response restarts the clock |
| 4 | `UNAVAILABLE` | server, when the agent's session ends, it stops advertising, or the request is dropped from its full backlog |

Limits: a requester may have 64 open requests, an agent 256, and the server 4,096; a request id may not be reused while its request is open.

`AGENT_COMMAND` (0x03) is the older, untyped form: it broadcasts `"<agent> <command>"` to `#lobby` as `BROADCAST_AGENT` and is kept for compatibility.

## Direct messages

`SEND` with kind 1 delivers to one user, online or within their hold period. The sender gets `SENT`; the recipient gets a `CHAT` whose target is their own username. A direct message to yourself is accepted and stored (you get `SENT`) but not echoed back.

## Error codes

| Code | Name | When |
|------|------|------|
| 0x0001 | `USERNAME_TAKEN` | name in use; the connection is closed |
| 0x0002 | `INVALID_FORMAT` | malformed frame, unknown type, already registered, message too long |
| 0x0003 | `SESSION_EXPIRED` | resume with an unknown or expired session id, or another user's |
| 0x0004 | `SERVER_ERROR` | no free connection or session slot, out of memory, too many rooms |
| 0x0005 | `NOT_REGISTERED` | anything but `REGISTER` before registering |
| 0x0006 | `NO_SUCH_TARGET` | unknown room or user |
| 0x0007 | `NOT_A_MEMBER` | send, history or leave without membership |
| 0x0008 | `INVALID_NAME` | bad username or room name |
| 0x0009 | `UNAUTHORIZED` | missing or wrong token for this username; the connection is closed |
| 0x000A | `NOT_AN_AGENT` | request to a user who has not advertised as an agent |
| 0x000B | `BAD_REQUEST` | request id already open, too many open requests, or a response to no open request |
