"""Cross-platform end-to-end smoke test (Linux, macOS, Windows).

    python3 tests/smoke.py path/to/tundra_chat_server[.exe]

Starts the server itself, exercises the main features over TCP, WebSocket
and the Python SDK, checks a clean shutdown (SIGINT on POSIX, Ctrl+Break on
Windows) and that history survives a restart. Exits non-zero on failure.
"""
import asyncio
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "sdk", "python"))
from tundra_raw import Conn, WsConn, frame, register, s8, s16, error_code  # noqa: E402
import tundra  # noqa: E402

SERVER = sys.argv[1]
WINDOWS = os.name == "nt"
results = []


def check(name, cond):
    results.append(bool(cond))
    print(("ok   - " if cond else "FAIL - ") + name, flush=True)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start(port, wsport, db):
    kwargs = {"stderr": subprocess.PIPE}
    if WINDOWS:
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen([SERVER, "-p", str(port), "-w", str(wsport), "-d", db], **kwargs)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("server did not start")


def stop(proc):
    if WINDOWS:
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        proc.send_signal(signal.SIGINT)
    try:
        code = proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = None
    err = proc.stderr.read().decode(errors="replace")
    return code, err


def chat_text(payload):
    tl = payload[17]
    sl = payload[18 + tl]
    ml = struct.unpack(">H", payload[19 + tl + sl:21 + tl + sl])[0]
    return payload[21 + tl + sl:21 + tl + sl + ml].decode()


async def sdk_checks(port):
    agent = tundra.Agent("127.0.0.1", port, "smokeagent", capabilities={"skills": ["upper"]})

    @agent.handler
    async def handle(req):
        yield "working"
        yield req.body.upper()

    await agent.connect()
    client = tundra.Client("127.0.0.1", port, "smokesdk")
    await client.connect()
    check("sdk: agent is listed", "smokeagent" in await client.agents())
    parts = [p async for p in client.request_stream("smokeagent", "hello")]
    check("sdk: streamed agent answer", parts == ["working", "HELLO"])
    await client.close()
    await agent.close()


def main():
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "smoke.db")
    port, wsport = free_port(), free_port()
    proc = start(port, wsport, db)

    # register, rooms, chat, DM, history
    a = Conn(port)
    a.send(register(b"alice"))
    sid = a.wait_for(0x10)[1][:16]
    check("register over TCP", len(sid) == 16)
    b = Conn(port)
    b.send(register(b"bob"))
    b.wait_for(0x10)
    for c in (a, b):
        c.send(frame(0x06, s8(b"#smoke")))
        c.wait_for(0x1A)
    time.sleep(0.2)
    a.send(frame(0x08, bytes([0]) + s8(b"#smoke") + s16(b"hello room")))
    f = b.wait_for(0x17)
    check("room message delivered", f is not None and chat_text(f[1]) == "hello room")
    a.send(frame(0x08, bytes([1]) + s8(b"bob") + s16(b"hello dm")))
    f = b.wait_for(0x17)
    check("direct message delivered", f is not None and chat_text(f[1]) == "hello dm")
    b.send(frame(0x0B, s8(b"#smoke") + struct.pack(">QH", 0, 10)))
    f = b.wait_for(0x1B)
    check("history answered", f is not None and struct.unpack(">H", f[1][7:9])[0] == 1)

    # resume with catch-up
    a.sock.close()
    time.sleep(0.3)
    b.send(frame(0x08, bytes([0]) + s8(b"#smoke") + s16(b"while away")))
    b.wait_for(0x18)
    a = Conn(port)
    a.send(register(b"alice", sid))
    a.wait_for(0x10)
    f = a.wait_for(0x17)
    check("resume catches up", f is not None and chat_text(f[1]) == "while away")

    # WebSocket
    w = WsConn(wsport)
    check("websocket handshake", "101 Switching Protocols" in w.handshake)
    w.send(register(b"webby"))
    check("register over WebSocket", w.recv_frame()[0] == 0x10)

    # errors
    d = Conn(port)
    d.send(register(b"bob"))
    f = d.wait_for(0x15)
    check("duplicate name refused", f is not None and error_code(f[1]) == 1)

    asyncio.run(sdk_checks(port))

    # session ids are random version-4 UUIDs (the built-in generator)
    u = uuid.UUID(bytes=bytes(sid))
    check("session id is a v4 UUID", u.version == 4 and u.variant == uuid.RFC_4122)

    # many clients at once: more ready sockets than one poll batch
    many = []
    for i in range(120):
        m = Conn(port)
        m.send(register(b"many%03d" % i))
        many.append(m)
    ok = sum(1 for m in many if m.wait_for(0x10, 5.0) is not None)
    check("120 concurrent clients register", ok == 120)
    for m in many:
        m.send(frame(0x06, s8(b"#crowd")))
    ok = sum(1 for m in many if m.wait_for(0x1A, 5.0) is not None)
    check("120 concurrent clients join a room", ok == 120)
    time.sleep(0.3)
    many[0].send(frame(0x08, bytes([0]) + s8(b"#crowd") + s16(b"to everyone")))
    got = [None] * len(many)

    def recv_one(i):
        f = many[i].wait_for(0x17, 5.0)
        got[i] = f is not None and chat_text(f[1]) == "to everyone"
    ts = [threading.Thread(target=recv_one, args=(i,)) for i in range(1, len(many))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    check("a room message fans out to 119 members", all(got[1:]))
    for m in many:
        m.sock.close()

    # a slow reader with a large backlog: the server queues and paces it
    slow = Conn(port)
    slow.send(register(b"slowpoke"))
    slow.wait_for(0x10)
    slow.send(frame(0x06, s8(b"#flood")))
    slow.wait_for(0x1A)
    fast = Conn(port)
    fast.send(register(b"flooder"))
    fast.wait_for(0x10)
    fast.send(frame(0x06, s8(b"#flood")))
    fast.wait_for(0x1A)
    time.sleep(0.2)
    big = b"x" * 4000
    burst = b"".join(frame(0x08, bytes([0]) + s8(b"#flood") + s16(big + b"%04d" % i)) for i in range(400))
    fast.send(burst)
    time.sleep(1.0)  # let the slow reader's queue build up
    n = 0
    while True:
        f = slow.wait_for(0x17, 3.0)
        if f is None:
            break
        if chat_text(f[1]).endswith("%04d" % n):
            n += 1
        time.sleep(0.001)
    check("a slow reader drains a 1.6 MB backlog in order", n == 400)
    slow.sock.close()
    fast.sock.close()

    for c in (a, b, w):
        c.sock.close()
    code, err = stop(proc)
    check("clean shutdown", code == 0)
    check("no failures logged", "failed" not in err.lower())
    if code != 0 or "failed" in err.lower():
        print(err)

    # persistence across a restart
    proc = start(port, wsport, db)
    h = Conn(port)
    h.send(register(b"hank"))
    h.wait_for(0x10)
    h.send(frame(0x06, s8(b"#smoke")))
    h.wait_for(0x1A)
    h.send(frame(0x0B, s8(b"#smoke") + struct.pack(">QH", 0, 10)))
    f = h.wait_for(0x1B)
    check("history survives a restart", f is not None and struct.unpack(">H", f[1][7:9])[0] == 2)
    h.sock.close()
    code, err = stop(proc)
    check("clean shutdown after restart", code == 0)

    print("%d/%d smoke checks passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
