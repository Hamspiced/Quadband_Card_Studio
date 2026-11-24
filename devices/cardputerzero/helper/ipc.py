"""Private inherited-pipe protocol. No reader commands originate here."""
import asyncio
import os
import struct

VERSION = 1
LIMIT = 8192
HEADER = struct.Struct("!BBII")
SCAN, CONNECT, EXCHANGE, DISCONNECT, PREFLIGHT, PIN = range(1, 7)
RESULT, EVENT, ERROR = 128, 129, 130


class ProtocolError(Exception):
    pass


def encode(kind, session, request, payload=b""):
    if (type(kind) is not int or kind not in
            (SCAN, CONNECT, EXCHANGE, DISCONNECT, PREFLIGHT, PIN, RESULT, EVENT, ERROR) or
            type(session) is not int or not 0 < session < 2**32 or
            type(request) is not int or not 0 < request < 2**32 or
            not isinstance(payload, bytes) or len(payload) > LIMIT - HEADER.size):
        raise ProtocolError("invalid envelope")
    body = HEADER.pack(VERSION, kind, session, request) + payload
    return struct.pack("!I", len(body)) + body


class Pipe:
    def __init__(self, read_fd, write_fd):
        self.read_fd, self.write_fd = read_fd, write_fd
        self.buffer = bytearray()
        self.queue = asyncio.Queue(maxsize=4)
        self.loop = asyncio.get_running_loop()
        os.set_blocking(read_fd, False)
        os.set_blocking(write_fd, False)
        self.loop.add_reader(read_fd, self._read)

    def _read(self):
        try:
            data = os.read(self.read_fd, LIMIT + 4)
            if not data or len(self.buffer) + len(data) > 2 * (LIMIT + 4):
                raise ProtocolError("closed or overflowing pipe")
            self.buffer.extend(data)
            while len(self.buffer) >= 4:
                size = struct.unpack_from("!I", self.buffer)[0]
                if not HEADER.size <= size <= LIMIT:
                    raise ProtocolError("invalid length")
                if len(self.buffer) < size + 4:
                    break
                version, kind, session, request = HEADER.unpack_from(self.buffer, 4)
                payload = bytes(self.buffer[4 + HEADER.size:4 + size])
                del self.buffer[:size + 4]
                if version != VERSION:
                    raise ProtocolError("invalid version")
                encode(kind, session, request, payload)
                self.queue.put_nowait((kind, session, request, payload))
        except (OSError, ProtocolError, asyncio.QueueFull):
            self.loop.remove_reader(self.read_fd)
            # A full queue is fatal, never drop a command or replay it later.
            while not self.queue.empty():
                self.queue.get_nowait()
            self.queue.put_nowait(None)

    async def receive(self):
        result = await self.queue.get()
        if result is None:
            raise ProtocolError("pipe failed")
        return result

    async def send(self, kind, session, request, payload=b""):
        data = encode(kind, session, request, payload)
        due = self.loop.time() + 2
        while data:
            try:
                count = os.write(self.write_fd, data)
                if count <= 0:
                    raise ProtocolError("pipe failed")
                data = data[count:]
            except BlockingIOError:
                if self.loop.time() >= due:
                    raise ProtocolError("pipe backpressure")
                await asyncio.sleep(.01)

    def close(self):
        self.loop.remove_reader(self.read_fd)
        os.close(self.read_fd)
        os.close(self.write_fd)
