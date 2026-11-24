"""Device-free private-pipe fixture; no Bluetooth imports."""
import os
import struct
import sys
import time

r, w = map(int, sys.argv[1:])
close_fail = False


def exact(size):
    result = b""
    while len(result) < size:
        data = os.read(r, size - len(result))
        if not data:
            sys.exit(0)
        result += data
    return result


def send(kind, session, request, data=b""):
    body = struct.pack("!BBII", 1, kind, session, request) + data
    os.write(w, struct.pack("!I", len(body)) + body)


while True:
    size = struct.unpack("!I", exact(4))[0]
    body = exact(size)
    version, kind, session, request = struct.unpack_from("!BBII", body)
    payload = body[10:]
    if kind == 5:
        send(128, session, request, b"mock")
    elif kind == 4:
        send(130 if close_fail else 128, session, request)
    elif kind == 1:
        send(128, session, request, b"\x03" + b"a" * 16 + b"\x03One" + b"b" * 16 + b"\x03Two" +
             b"d" * 16 + b"\x04Fail")
    elif kind == 2:
        close_fail = payload == b"d" * 16
        if payload == b"c" * 16:
            for _ in range(33):
                send(129, session, request, b"connecting")
        if payload == b"b" * 16:
            send(129, session, request, b"pin")
            pin = exact(struct.unpack("!I", exact(4))[0])
            if len(pin) != 16 or pin[1] != 6 or pin[10:].isdigit() is False:
                sys.exit(1)
        send(128, session, request)
    elif kind == 3:
        frame = payload[4:]
        command = int.from_bytes(frame[2:4], "big")
        if command == 2000:
            send(128, session + 1, request)
        elif command == 2001:
            os.write(w, struct.pack("!I", 8193))
        elif command == 2002:
            sys.exit(0)
        elif command == 2003:
            time.sleep(2)
        else:
            reply = bytearray(frame)
            reply[4:6] = b"\x00\x68"
            reply[8] = 0
            reply[8] = -sum(reply[:9]) % 256
            if command == 2005:
                reply[8] ^= 1
            data = struct.pack("!BBII", 1, 128, session, request) + bytes(reply)
            envelope = struct.pack("!I", len(data)) + data
            for byte in envelope:
                os.write(w, bytes([byte]))
            if command == 2006:
                send(128, session, request, b"late")
