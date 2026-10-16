"""Minimal raw-frame helper for tests: build and read Tundra frames by hand."""
import socket
import struct
import time


def frame(ftype, payload=b"", seq=0, version=1, length=None):
    if length is None:
        length = len(payload)
    return struct.pack(">BBHHB", version, 0, seq, length, ftype) + payload


def s8(b):
    return bytes([len(b)]) + b


def s16(b):
    return struct.pack(">H", len(b)) + b


def register(name, sid=bytes(16)):
    return frame(0x01, sid + s8(name))


class Conn:
    def __init__(self, port, timeout=2.0):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.sock.settimeout(timeout)
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def recv_frame(self):
        """Returns (type, payload), or None on timeout/close."""
        while True:
            if len(self.buf) >= 7:
                length = struct.unpack(">H", self.buf[4:6])[0]
                if len(self.buf) >= 7 + length:
                    t, p = self.buf[6], self.buf[7:7 + length]
                    self.buf = self.buf[7 + length:]
                    return t, p
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk

    def wait_for(self, ftype, deadline=2.0):
        end = time.time() + deadline
        while time.time() < end:
            f = self.recv_frame()
            if f is None:
                return None
            if f[0] == ftype:
                return f
        return None

    def closed(self, wait=1.0):
        """True if the server closes the connection within `wait` seconds."""
        self.sock.settimeout(wait)
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return True
        except (socket.timeout, ConnectionResetError):
            return False


def error_code(payload):
    return struct.unpack(">H", payload[:2])[0]


class WsConn(Conn):
    """Same interface as Conn, but speaks WebSocket: each Tundra frame is one
    masked binary message. `handshake` holds the server's 101 response."""

    def __init__(self, port, path="/ws", key="dGhlIHNhbXBsZSBub25jZQ==", timeout=2.0):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.sock.settimeout(timeout)
        self.raw = b""
        self.buf = b""
        req = ("GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, key)).encode()
        self.sock.sendall(req)
        while b"\r\n\r\n" not in self.raw:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            self.raw += chunk
        head, _, rest = self.raw.partition(b"\r\n\r\n")
        self.handshake = head.decode(errors="replace")
        self.raw = rest

    @staticmethod
    def ws_frame(payload, opcode=0x2, fin=True, mask=b"\x11\x22\x33\x44"):
        first = (0x80 if fin else 0) | opcode
        n = len(payload)
        if n < 126:
            head = bytes([first, 0x80 | n])
        elif n <= 0xFFFF:
            head = bytes([first, 0x80 | 126]) + struct.pack(">H", n)
        else:
            head = bytes([first, 0x80 | 127]) + struct.pack(">Q", n)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return head + mask + body

    def send(self, data):
        self.sock.sendall(self.ws_frame(data))

    def recv_ws(self):
        """Returns (opcode, payload) of the next WebSocket message, or None."""
        while True:
            if len(self.raw) >= 2:
                n = self.raw[1] & 0x7F
                hdr = 2
                if n == 126 and len(self.raw) >= 4:
                    n = struct.unpack(">H", self.raw[2:4])[0]; hdr = 4
                elif n == 127 and len(self.raw) >= 10:
                    n = struct.unpack(">Q", self.raw[2:10])[0]; hdr = 10
                if (self.raw[1] & 0x7F) < 126 or hdr > 2:
                    if len(self.raw) >= hdr + n:
                        op, payload = self.raw[0] & 0x0F, self.raw[hdr:hdr + n]
                        self.raw = self.raw[hdr + n:]
                        return op, payload
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.raw += chunk

    def recv_frame(self):
        while True:
            m = self.recv_ws()
            if m is None:
                return None
            op, payload = m
            if op == 0x2 and len(payload) >= 7:
                return payload[6], payload[7:]
            if op == 0x8:
                return None
