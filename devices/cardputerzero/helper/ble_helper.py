#!/usr/bin/env python3
"""Unprivileged BLE transport, reachable only over two inherited private pipes."""
import asyncio
import importlib.metadata
import os
from pathlib import Path
import re
import secrets
import signal
import struct
import sys

# -I excludes ambient PYTHONPATH/current directory; only the installed payload is added.
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from ipc import (CONNECT, DISCONNECT, ERROR, EVENT, EXCHANGE, PIN, PREFLIGHT,  # noqa: E402
                 RESULT, SCAN, Pipe, ProtocolError)

PHASES = {"preflight", "scanning", "select", "pairing", "pin", "connecting",
          "subscribing", "validating", "exchange", "disconnect"}


def failure_payload(phase, error):
    """Send only fixed categories, never backend text, peer metadata or PINs."""
    if phase not in PHASES:
        phase = "preflight"
    if isinstance(error, (TimeoutError, asyncio.TimeoutError)):
        reason = "timeout"
    elif isinstance(error, ImportError) or phase == "preflight":
        reason = "dependencies"
    elif isinstance(error, ProtocolError):
        reason = "protocol"
    elif isinstance(error, (AttributeError, TypeError, NotImplementedError)):
        reason = "compatibility"
    elif type(error).__name__ == "BluetoothCleanupError":
        reason = "cleanup"
    else:
        name = getattr(error, "type", None) or getattr(error, "dbus_error", None)
        reasons = {
            "org.bluez.Error.AuthenticationFailed": "authentication",
            "org.bluez.Error.AuthenticationCanceled": "authentication",
            "org.bluez.Error.AuthenticationRejected": "authentication",
            "org.bluez.Error.Rejected": "authentication",
            "org.bluez.Error.AuthenticationTimeout": "timeout",
            "org.bluez.Error.InProgress": "busy",
            "org.bluez.Error.AlreadyConnected": "busy",
            "org.bluez.Error.NotReady": "unavailable",
            "org.bluez.Error.NotConnected": "unavailable",
            "org.bluez.Error.ConnectionAttemptFailed": "unavailable",
            "org.bluez.Error.NotPermitted": "permission",
            "org.bluez.Error.NotAuthorized": "permission",
            "org.freedesktop.DBus.Error.AccessDenied": "permission",
            "org.freedesktop.DBus.Error.NoReply": "timeout",
            "org.freedesktop.DBus.Error.UnknownObject": "unavailable",
            "org.freedesktop.DBus.Error.UnknownMethod": "compatibility",
            "org.freedesktop.DBus.Error.UnknownInterface": "compatibility",
        }
        reason = reasons.get(name, "transport") if isinstance(name, str) else "transport"
    return f"ble:{phase}:{reason}".encode("ascii")


def preflight():
    if sys.platform != "linux":
        raise RuntimeError("linux_required")
    if os.getuid() == 0 or os.geteuid() == 0:
        raise RuntimeError("unprivileged_user_required")
    import bleak
    import dbus_fast
    del bleak, dbus_fast
    version = importlib.metadata.version("bleak")
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:[.+-].*)?", version)
    if not match or tuple(map(int, match.groups())) < (0, 22, 3):
        raise RuntimeError("bleak_0_22_3_required")
    return b"python3 bluez bleak>=0.22.3 dbus-fast"


class Session:
    def __init__(self, pipe):
        self.pipe = pipe
        self.session = self.request = 0
        self.devices = {}
        self.channel = self.agent = None
        self.active = None
        self.failed = False
        self.death = asyncio.Event()
        self.discovered_at = 0
        self.pin_challenge = False
        self.phase = "preflight"

    async def event(self, name):
        if name in PHASES:
            self.phase = name
        await self.pipe.send(EVENT, self.session, self.request, name.encode("ascii"))

    def set_agent(self, agent):
        self.agent = agent

    async def operation(self, kind, data):
        if kind == PREFLIGHT:
            if data:
                raise ProtocolError("unexpected payload")
            return preflight()
        preflight()
        from quadband.bluetooth import BLEChannel, ble_devices
        if kind == SCAN:
            if data or self.channel is not None:
                raise ProtocolError("scan while connected")
            self.devices.clear()
            self.discovered_at = asyncio.get_running_loop().time()
            await self.event("scanning")
            devices = await asyncio.wait_for(ble_devices(chameleon=True, timeout=6), 8)
            records = []
            for device, label in list(devices)[:16]:
                token = secrets.token_hex(8)
                name = str(label or getattr(device, "name", None) or "Unnamed BLE reader")
                name = "".join(c if 32 <= ord(c) < 127 else "?" for c in name)[:64]
                self.devices[token] = device
                encoded = name.encode("ascii")
                records.append(token.encode("ascii") + bytes([len(encoded)]) + encoded)
            await self.event("select")
            return bytes([len(records)]) + b"".join(records)
        if kind == CONNECT:
            if self.channel is not None or len(data) != 16:
                raise ProtocolError("invalid selection")
            token = data.decode("ascii")
            device = self.devices.get(token)
            if device is None or asyncio.get_running_loop().time() - self.discovered_at > 60:
                raise ProtocolError("expired selection")
            self.devices.clear()
            from bluez_pair import pair_selected
            await self.event("pairing")
            async def prompt_pin():
                if self.pin_challenge:
                    raise ProtocolError("repeated pairing challenge")
                self.pin_challenge = True
                await self.event("pin")
            await pair_selected(device, prompt_pin, self.set_agent)
            await self.event("connecting")
            subscription = None

            def progress(state):
                nonlocal subscription
                if state.startswith("Subscribing") and subscription is None:
                    subscription = asyncio.create_task(self.event("subscribing"))

            try:
                self.channel = await BLEChannel.connect(
                    device, chameleon=True, progress=progress,
                    pair=False, on_disconnect=self.disconnected)
            finally:
                if subscription is not None:
                    await subscription
            await self.event("validating")
            return b""
        if kind == EXCHANGE:
            if self.channel is None or len(data) < 14 or len(data) > 4100 or self.failed:
                raise ProtocolError("invalid exchange")
            timeout = struct.unpack_from("!I", data)[0]
            frame = data[4:]
            if not 1 <= timeout <= 20000 or frame[:2] != b"\x11\xef":
                raise ProtocolError("invalid exchange")
            if not self.channel.queue.empty():
                raise ProtocolError("unsolicited or late notification")
            due = asyncio.get_running_loop().time() + timeout / 1000
            await asyncio.wait_for(self.channel.send(frame), timeout / 1000)
            response = bytearray()
            while True:
                left = due - asyncio.get_running_loop().time()
                if left <= 0 or self.failed:
                    raise RuntimeError("reply_timeout_or_disconnect")
                fragment = await asyncio.wait_for(self.channel.receive(left), left)
                if not fragment or len(response) + len(fragment) > 4096:
                    raise ProtocolError("invalid notification")
                response.extend(fragment)
                if len(response) >= 9:
                    total = int.from_bytes(response[6:8], "big") + 10
                    if total > 4096 or len(response) > total:
                        raise ProtocolError("invalid reply length")
                    if len(response) == total:
                        if not self.channel.queue.empty():
                            raise ProtocolError("extra notification")
                        return bytes(response)
        if kind == DISCONNECT:
            if data:
                raise ProtocolError("unexpected payload")
            await self.close()
            return b""
        raise ProtocolError("unknown request")

    def disconnected(self, *_):
        self.failed = True
        self.death.set()

    async def close(self):
        channel, self.channel = self.channel, None
        self.devices.clear()
        if channel is not None:
            await asyncio.wait_for(channel.close(), 6)

    async def run(self):
        receiving = asyncio.create_task(self.pipe.receive())
        death = asyncio.create_task(self.death.wait())
        try:
            while True:
                waiting = {receiving, death}
                if self.active is not None:
                    waiting.add(self.active)
                done, _ = await asyncio.wait(waiting, return_when=asyncio.FIRST_COMPLETED)
                if death in done:
                    raise ProtocolError("disconnected")
                if self.active in done:
                    await self.active
                    self.active = None
                if receiving not in done:
                    continue
                kind, session, request, data = receiving.result()
                receiving = asyncio.create_task(self.pipe.receive())
                if kind == PIN:
                    if (session != self.session or request != self.request or self.agent is None or
                            len(data) != 6 or not data.isdigit() or not self.agent.answer(data.decode("ascii"))):
                        raise ProtocolError("unexpected PIN response")
                    continue
                if (self.active is not None or kind not in (SCAN, CONNECT, EXCHANGE, DISCONNECT, PREFLIGHT) or
                        (self.session and session != self.session) or request <= self.request):
                    raise ProtocolError("invalid session/request")
                self.session, self.request = session, request
                self.active = asyncio.create_task(self.reply(kind, data))
        finally:
            receiving.cancel()
            death.cancel()
            await asyncio.gather(receiving, death, return_exceptions=True)
            if self.active is not None:
                self.active.cancel()
                try:
                    await self.active
                except BaseException:
                    pass
            try:
                await self.close()
            finally:
                self.pipe.close()

    async def reply(self, kind, data):
        self.phase = {PREFLIGHT: "preflight", SCAN: "scanning", CONNECT: "select",
                      EXCHANGE: "exchange", DISCONNECT: "disconnect"}.get(kind, "preflight")
        try:
            result = await asyncio.wait_for(self.operation(kind, data), 55 if kind == CONNECT else 25)
            await self.pipe.send(RESULT, self.session, self.request, result)
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.failed = True
            # Never serialize backend exceptions: some include passkeys or raw peer metadata.
            await self.pipe.send(ERROR, self.session, self.request, failure_payload(self.phase, error))
            raise ProtocolError("operation failed") from None


async def main():
    if len(sys.argv) != 3 or not all(s.isdecimal() for s in sys.argv[1:]):
        raise ProtocolError("inherited descriptors required")
    task = asyncio.current_task()
    asyncio.get_running_loop().add_signal_handler(signal.SIGTERM, task.cancel)
    await Session(Pipe(int(sys.argv[1]), int(sys.argv[2]))).run()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (Exception, asyncio.CancelledError):
        sys.exit(1)
