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
