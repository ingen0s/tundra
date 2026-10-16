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

## Field notation

| Notation | Meaning |
|----------|---------|
| `u8`, `u16`, `u32`, `u64` | unsigned big-endian integers |
| `str8` | `u8` length, then that many bytes |
| `str16` | `u16` length, then that many bytes |
| `id16` | 16 raw bytes (a UUID) |

Text is UTF-8 by convention; the server does not inspect it. Chat text is limited to 65,450 bytes so that it fits in every frame that carries it.

## Names

- **Usernames:** 1 to 32 printable ASCII characters, no spaces, must not start with `#`.
- **Rooms:** `#` followed by 1 to 31 of `a-z 0-9 _ -`.

## Client to server

| Type | Code | Payload |
|------|------|---------|
| `REGISTER` | 0x01 | `id16 id` `str8 username`. Must be sent within 10 seconds of connecting. |
| `MESSAGE` | 0x02 | `str16 text`. Shorthand for `SEND` to `#lobby`. |
| `AGENT_COMMAND` | 0x03 | `str8 agent` `str16 command`. Legacy; broadcast to `#lobby` as `BROADCAST_AGENT`. |
| `PONG` | 0x04 | echo of the `PING` payload. Required: it keeps the connection alive and latency is computed from the echoed timestamp. |
| `HEALTH_CHECK` | 0x05 | empty |
| `JOIN` | 0x06 | `str8 room`. Creates the room if needed. |
| `LEAVE` | 0x07 | `str8 room` |
| `SEND` | 0x08 | `u8 kind` `str8 target` `str16 text`. Kind 0 = room (target is a room), 1 = direct (target is a username). |
| `LIST_ROOMS` | 0x09 | empty |
| `HISTORY` | 0x0B | `str8 room` `u64 before_id` `u16 limit`. `before_id` 0 means "latest". `limit` 0 or over 100 means 100. |
| `WHO` | 0x0C | `str8 room` |

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

`BROADCAST_MESSAGE` (0x11) and `IMAGE` (0x13) are reserved and not sent.

Message ids are assigned by the server, are unique, and increase by one per message across all rooms and direct messages.

## Sessions

1. `REGISTER` with an all-zero id. The welcome `SYSTEM` frame carries your session id, and you are joined to `#lobby` (you get a `ROOM_EVENT` for it).
2. If the connection drops, the session, its username and its room memberships are held for 2 minutes. Frames for it are buffered: up to 50 frames and 1 MiB per session (64 MiB across the server), oldest dropped first.
3. `REGISTER` with the same id and username to resume. The backlog is delivered right after the welcome, paced so it never overflows the connection; if anything was dropped, a `SYSTEM` notice says how many. If the old connection is still open it is closed. An unknown or expired id, or someone else's id, gets `SESSION_EXPIRED`: register again with a zero id.
4. After the hold period the session is released, its username becomes free, and each of its rooms gets a "left" event. When the session table is full, the longest-detached session is released early to make room.

Registered connections that send nothing for 2 minutes are dropped; the server pings every 30 seconds, so answering with `PONG` is enough. Each client's outgoing queue is capped at 1 MiB; a client that falls further behind is disconnected and loses what was still queued for it.

## Rooms

- `JOIN` creates a room if it does not exist. Members, including the joiner, get `ROOM_EVENT joined`.
- `LEAVE` sends `ROOM_EVENT left` to the remaining members and the leaver. Empty rooms are deleted, except `#lobby`.
- Room messages go to every member except the sender. Members who are disconnected get them in their backlog.
- `HISTORY` and sending require membership. `WHO` and `LIST_ROOMS` do not.
- The server keeps the most recent 10,000 messages (and at most 64 MiB of text) in memory for `HISTORY`.

## Direct messages

`SEND` with kind 1 delivers to one user, online or within their hold period. The sender gets `SENT`; the recipient gets a `CHAT` whose target is their own username.

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
