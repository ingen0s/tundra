"""Tundra client SDK for Python (asyncio, no dependencies).

    import asyncio, tundra

    async def main():
        client = tundra.Client("localhost", 4242, "alice")
        await client.connect()
        await client.join("#dev")
        await client.send("#dev", "hello")
        async for msg in client.messages():
            print(msg.sender, msg.text)

    asyncio.run(main())

Agents:

    agent = tundra.Agent("localhost", 4242, "summarizer", capabilities={"skills": ["summarize"]})

    @agent.handler
    async def handle(req):
        return "summary of: " + req.body          # or `yield` partial answers

    asyncio.run(agent.run_forever())

The client answers pings, acknowledges messages, and reconnects with
session resume (catching up on anything missed) when the connection drops.
"""
import asyncio
import inspect
import json
import struct
from dataclasses import dataclass
from typing import Optional

__all__ = ["Client", "Agent", "Chat", "AgentRequest", "AgentError", "TundraError"]

VERSION = 1

# client -> server
REGISTER, MESSAGE, PONG, HEALTH_CHECK = 0x01, 0x02, 0x04, 0x05
JOIN, LEAVE, SEND, LIST_ROOMS, ACK, HISTORY, WHO = 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C
AGENT_ADVERTISE, AGENT_REQUEST, AGENT_RESPONSE, LIST_AGENTS = 0x20, 0x21, 0x22, 0x23
# server -> client
SYSTEM, PING, ERROR, HEALTH_RESULT = 0x10, 0x14, 0x15, 0x16
CHAT, SENT, ROOM_LIST, ROOM_EVENT, HISTORY_RESULT, WHO_RESULT = 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C
AGENT_LIST, AGENT_REQUEST_IN, AGENT_RESPONSE_IN = 0x30, 0x31, 0x32

REG_RESUME_FROM, REG_TOKEN = 0x01, 0x02
# errors that retrying cannot fix: name taken, invalid name, unauthorized
FATAL_ERRORS = {1, 8, 9}
STATUS = {0: "ok", 1: "error", 2: "rejected", 3: "timeout", 4: "unavailable"}
REPLY_TYPES = {SYSTEM, ERROR, SENT, ROOM_LIST, WHO_RESULT, HISTORY_RESULT, HEALTH_RESULT, AGENT_LIST}


class TundraError(Exception):
    """An ERROR frame from the server."""

    def __init__(self, code, text):
        super().__init__("%d: %s" % (code, text))
        self.code, self.text = code, text


class AgentError(Exception):
    """An agent answered with a non-ok status, or the server closed the request."""

    def __init__(self, agent, status, body):
        super().__init__("%s answered %s: %s" % (agent, STATUS.get(status, status), body))
        self.agent, self.status, self.body = agent, status, body


@dataclass
class Chat:
    id: int
    ts_ms: int
    room: Optional[str]   # set for room messages, None for direct messages
    sender: str
    text: str
    direct: bool = False  # True for direct messages (room is None)


@dataclass
class AgentRequest:
    sender: str
    id: int
    body: str


# ---- encoding helpers ----

def _s8(s):
    b = s.encode() if isinstance(s, str) else s
    if len(b) > 255:
        raise ValueError("field longer than 255 bytes")
    return bytes([len(b)]) + b


def _s16(s):
    b = s.encode() if isinstance(s, str) else s
    if len(b) > 0xFFFF:
        raise ValueError("field longer than 65535 bytes")
    return struct.pack(">H", len(b)) + b


class _R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def u8(self):
        self.p += 1
        return self.b[self.p - 1]

    def u16(self):
        self.p += 2
        return struct.unpack(">H", self.b[self.p - 2:self.p])[0]

    def u64(self):
        self.p += 8
        return struct.unpack(">Q", self.b[self.p - 8:self.p])[0]

    def raw(self, n):
        self.p += n
        return self.b[self.p - n:self.p]

    def s8(self):
        return self.raw(self.u8()).decode(errors="replace")

    def s16(self):
        return self.raw(self.u16()).decode(errors="replace")


def _read_chat(r):
    cid, ts, kind = r.u64(), r.u64(), r.u8()
    target, sender, text = r.s8(), r.s8(), r.s16()
    if kind == 0:
        return Chat(cid, ts, target, sender, text)
    return Chat(cid, ts, None, sender, text, direct=True)


class Client:
    def __init__(self, host="127.0.0.1", port=4242, username="", token=None, reconnect=True, ssl=None):
        """`ssl`: None for plain TCP, True for TLS verified against the system
        CAs, or an ssl.SSLContext (for example one that trusts a private CA)."""
        self.host, self.port, self.username, self.token = host, port, username, token
        self.ssl = ssl
        self.reconnect = reconnect
        self.session_id = None
        self.last_id = 0
        self._seq = 0
        self._next_req = 1
        self._reader = self._writer = None
        self._replies = {}            # seq -> future
        self._requests = {}           # req id -> queue of (status, final, body)
        self._inbox = asyncio.Queue()
        self._seen = set()
        self._task = None
        self._closing = False
        self._ack_handle = None
        self._connected = asyncio.Event()
        self.error = None             # set when the client gives up (see FATAL_ERRORS)

    # ---- connection ----

    def _register_payload(self):
        ext = b""
        if self.session_id and self.last_id:
            ext += bytes([REG_RESUME_FROM]) + struct.pack(">HQ", 8, self.last_id)
        if self.token:
            tok = self.token.encode()
            ext += bytes([REG_TOKEN]) + struct.pack(">H", len(tok)) + tok
        return (self.session_id or bytes(16)) + _s8(self.username) + ext

    async def _open(self):
        if self.ssl:
            ctx = self.ssl
            if ctx is True:
                import ssl as _ssl
                ctx = _ssl.create_default_context()
            return await asyncio.open_connection(self.host, self.port, ssl=ctx, server_hostname=self.host)
        return await asyncio.open_connection(self.host, self.port)

    def _drop_writer(self):
        if self._writer is not None:
            self._writer.close()
            self._writer = None

    async def connect(self):
        """Connects and registers (or resumes). Raises TundraError on refusal;
        a refused client does not keep reconnecting in the background."""
        self._closing = False
        self._reader, self._writer = await self._open()
        fut = self._send(REGISTER, self._register_payload(), expect_reply=True)
        if self._task is None or self._task.done():
            self._task = asyncio.ensure_future(self._read_loop())
        try:
            ftype, payload = await asyncio.wait_for(fut, 10)
            if ftype == ERROR:
                r = _R(payload)
                code, text = r.u16(), r.s16()
                if code == 3:  # session expired: the next attempt registers fresh
                    self.session_id = None
                raise TundraError(code, text)
        except BaseException:
            self._closing = True
            if self._task:
                self._task.cancel()
                self._task = None
            self._drop_writer()
            raise
        self.session_id = bytes(payload[:16])
        self._connected.set()
        await self._on_connected()

    async def _on_connected(self):
        pass

    async def close(self):
        self._closing = True
        if self._ack_handle is not None:
            self._ack_handle.cancel()
            self._ack_handle = None
        self._drop_writer()
        if self._task:
            self._task.cancel()

    def _send(self, ftype, payload=b"", expect_reply=False):
        if self._writer is None:
            raise ConnectionError("not connected")
        seq = self._seq & 0xFFFF
        self._seq += 1
        fut = None
        if expect_reply:
            fut = asyncio.get_running_loop().create_future()
            self._replies[seq] = fut
        self._writer.write(struct.pack(">BBHHB", VERSION, 0, seq, len(payload), ftype) + payload)
        return fut

    async def _request(self, ftype, payload=b""):
        ftype_r, body = await asyncio.wait_for(self._send(ftype, payload, expect_reply=True), 10)
        if ftype_r == ERROR:
            r = _R(body)
            raise TundraError(r.u16(), r.s16())
        return ftype_r, body

    async def _read_loop(self):
        while not self._closing:
            try:
                head = await self._reader.readexactly(7)
                payload = await self._reader.readexactly(struct.unpack(">H", head[4:6])[0])
            except (asyncio.IncompleteReadError, ConnectionError, OSError):
                if self._closing or not self.reconnect:
                    break
                await self._reconnect()
                continue
            seq, ftype = struct.unpack(">H", head[2:4])[0], head[6]
            self._dispatch(seq, ftype, payload)

    async def _reconnect(self):
        self._connected.clear()
        for fut in self._replies.values():
            if not fut.done():
                fut.set_exception(ConnectionError("connection lost"))
        self._replies.clear()
        self._drop_writer()
        delay = 0.2
        while not self._closing:
            await asyncio.sleep(delay)
            try:
                self._reader, self._writer = await self._open()
            except OSError:
                delay = min(delay * 2, 5)
                continue
            # register inline: the read loop is this coroutine
            fresh = self.session_id is None
            self._send(REGISTER, self._register_payload())
            try:
                head = await self._reader.readexactly(7)
                payload = await self._reader.readexactly(struct.unpack(">H", head[4:6])[0])
            except (asyncio.IncompleteReadError, ConnectionError, OSError):
                self._drop_writer()
                continue
            if head[6] == SYSTEM:
                self.session_id = bytes(payload[:16])
                if fresh:
                    # a new session: ids may restart (a server without a database)
                    self._seen.clear()
                    self.last_id = 0
                self._connected.set()
                asyncio.ensure_future(self._on_connected())
                return
            if head[6] == ERROR:
                r = _R(payload)
                code, text = r.u16(), r.s16()
                if code in FATAL_ERRORS:
                    # retrying cannot help: give up and let messages() end
                    self.error = TundraError(code, text)
                    self._closing = True
                    self._drop_writer()
                    self._inbox.put_nowait(None)
                    return
                if code == 3:
                    self.session_id = None  # expired: register fresh on the next try
            self._drop_writer()
            delay = min(delay * 2, 5)

    def _dispatch(self, seq, ftype, payload):
        if ftype in REPLY_TYPES and seq in self._replies:
            fut = self._replies.pop(seq)
            if not fut.done():
                fut.set_result((ftype, payload))
            return
        r = _R(payload)
        if ftype == PING:
            self._send(PONG, payload)
        elif ftype == CHAT:
            msg = _read_chat(r)
            if msg.id in self._seen:  # repeated by a catch-up replay
                return
            self._seen.add(msg.id)
            if len(self._seen) > 10000:
                self._seen = {i for i in self._seen if i > self.last_id - 5000}
            self.last_id = max(self.last_id, msg.id)
            self._schedule_ack()
            self._inbox.put_nowait(msg)
        elif ftype == AGENT_RESPONSE_IN:
            r.s8()
            rid, status, final, body = r.u64(), r.u8(), r.u8(), r.s16()
            q = self._requests.get(rid)
            if q:
                q.put_nowait((status, final, body))
        elif ftype == AGENT_REQUEST_IN:
            sender, rid, body = r.s8(), r.u64(), r.s16()
            self._on_agent_request(AgentRequest(sender, rid, body))
        # ROOM_EVENT, notices and unmatched replies are ignored by default

    def _on_agent_request(self, req):
        pass

    def _schedule_ack(self):
        if self._ack_handle is None:
            self._ack_handle = asyncio.get_running_loop().call_later(0.2, self._send_ack)

    def _send_ack(self):
        self._ack_handle = None
        if self._writer and self._connected.is_set():
            self._send(ACK, struct.pack(">Q", self.last_id))

    # ---- chat ----

    async def join(self, room):
        self._send(JOIN, _s8(room))

    async def leave(self, room):
        self._send(LEAVE, _s8(room))

    async def send(self, target, text):
        """Sends to a "#room" or a username. Returns the message id."""
        kind = 0 if target.startswith("#") else 1
        _, body = await self._request(SEND, bytes([kind]) + _s8(target) + _s16(text))
        return struct.unpack(">Q", body)[0]

    async def history(self, room, limit=50, before_id=0):
        _, body = await self._request(HISTORY, _s8(room) + struct.pack(">QH", before_id, limit))
        r = _R(body)
        r.s8()
        out = []
        for _ in range(r.u16()):
            out.append(_read_chat(_R(r.raw(r.u16()))))
        return out

    async def rooms(self):
        _, body = await self._request(LIST_ROOMS)
        r = _R(body)
        return {r.s8(): r.u16() for _ in range(r.u16())}

    async def messages(self):
        """Yields every chat message received, including catch-up after a resume.
        Ends if the client gives up reconnecting (see `error`)."""
        while True:
            msg = await self._inbox.get()
            if msg is None:
                if self.error:
                    raise self.error
                return
            yield msg

    # ---- agents ----

    async def agents(self):
        """Returns {name: {"online": bool, "capabilities": parsed JSON or str}}."""
        _, body = await self._request(LIST_AGENTS)
        r = _R(body)
        self.agents_truncated = bool(r.u8())  # more agents exist than fit in one frame
        out = {}
        for _ in range(r.u16()):
            name, online, caps = r.s8(), r.u8(), r.s16()
            try:
                caps = json.loads(caps)
            except ValueError:
                pass
            out[name] = {"online": bool(online), "capabilities": caps}
        return out

    async def request_stream(self, agent, body, timeout=120):
        """Sends a request and yields each answer (partial ones first). Raises
        AgentError when the agent or server answers with a non-ok status."""
        rid = self._next_req
        self._next_req += 1
        q = asyncio.Queue()
        self._requests[rid] = q
        # A refused request (unknown agent, not an agent...) comes back as an
        # ERROR reply to the request's sequence number.
        seq = self._seq & 0xFFFF
        refused = self._send(AGENT_REQUEST, _s8(agent) + struct.pack(">Q", rid) + _s16(body), expect_reply=True)
        try:
            while True:
                getter = asyncio.ensure_future(q.get())
                done, _ = await asyncio.wait({getter, refused}, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
                if refused in done:
                    getter.cancel()
                    _, payload = refused.result()
                    r = _R(payload)
                    raise TundraError(r.u16(), r.s16())
                if not done:
                    getter.cancel()
                    raise asyncio.TimeoutError()
                status, final, text = getter.result()
                if status != 0:
                    raise AgentError(agent, status, text)
                yield text
                if final:
                    return
        finally:
            self._requests.pop(rid, None)
            self._replies.pop(seq, None)
            if not refused.done():
                refused.cancel()

    async def request(self, agent, body, timeout=120):
        """Sends a request and returns the final answer."""
        last = None
        async for part in self.request_stream(agent, body, timeout):
            last = part
        return last


class Agent(Client):
    """A client that advertises capabilities and answers requests with a
    handler. The handler gets an AgentRequest and either returns the answer
    or is an async generator yielding partial answers (the last one is final)."""

    def __init__(self, host="127.0.0.1", port=4242, username="", capabilities=None, token=None, reconnect=True,
                 ssl=None):
        super().__init__(host, port, username, token, reconnect, ssl)
        self.capabilities = capabilities if capabilities is not None else {}
        self._handler = None

    def handler(self, fn):
        self._handler = fn
        return fn

    async def _on_connected(self):
        caps = self.capabilities if isinstance(self.capabilities, str) else json.dumps(self.capabilities)
        self._send(AGENT_ADVERTISE, _s16(caps or "{}"))

    def _respond(self, req, status, final, body):
        self._send(AGENT_RESPONSE, _s8(req.sender) + struct.pack(">QBB", req.id, status, final) + _s16(str(body)))

    def _on_agent_request(self, req):
        asyncio.ensure_future(self._handle(req))

    async def _handle(self, req):
        if self._handler is None:
            self._respond(req, 2, 1, "no handler")
            return
        try:
            result = self._handler(req)
            if inspect.isasyncgen(result):
                prev = None
                async for part in result:
                    if prev is not None:
                        self._respond(req, 0, 0, prev)
                    prev = part
                self._respond(req, 0, 1, "" if prev is None else prev)
            else:
                if inspect.isawaitable(result):
                    result = await result
                self._respond(req, 0, 1, "" if result is None else result)
        except Exception as e:  # report the failure to the requester
            self._respond(req, 1, 1, "%s: %s" % (type(e).__name__, e))

    async def run_forever(self):
        await self.connect()
        await asyncio.Event().wait()
