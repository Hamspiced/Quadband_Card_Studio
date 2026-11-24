import asyncio
import importlib
import importlib.metadata
import inspect
from pathlib import Path
import os
import struct
import subprocess
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch

ROOT = Path(__file__).resolve().parents[3]
HELPER = ROOT / "devices/cardputerzero/helper"
sys.path.insert(0, str(ROOT / "desktop/src"))
sys.path.insert(0, str(HELPER))
import ipc  # noqa: E402
import ble_helper as helper  # noqa: E402
from quadband import bluetooth  # noqa: E402


class FakePipe:
    def __init__(self):
        self.incoming = asyncio.Queue()
        self.sent = []
        self.closed = False

    async def receive(self):
        return await self.incoming.get()

    async def send(self, kind, session, request, data=b""):
        self.sent.append((kind, session, request, data))

    def close(self):
        self.closed = True


class HelperTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.pipe = FakePipe()
        self.s = helper.Session(self.pipe)
        self.s.session, self.s.request = 7, 1
        self.preflight = patch.object(helper, "preflight", return_value=b"mock dependencies")
        self.preflight.start()

    async def asyncTearDown(self):
        self.preflight.stop()

    async def test_real_shared_discovery_shape_retains_object_and_bounds(self):
        peers = [types.SimpleNamespace(name="reader", details={"path": "/fake"}) for _ in range(20)]
        found = [(peer, "Reader\n" + "x" * 100) for peer in peers]
        with patch.object(bluetooth, "ble_devices", AsyncMock(return_value=found)) as scan:
            result = await self.s.operation(ipc.SCAN, b"")
        scan.assert_awaited_once_with(chameleon=True, timeout=6)
        self.assertEqual(result[0], 16)
        self.assertEqual(len(self.s.devices), 16)
        self.assertEqual(list(self.s.devices.values()), peers[:16])
        self.assertNotIn(b"\n", result)
        self.assertLessEqual(len(result), 1 + 16 * 81)

    async def test_expired_or_arbitrary_selection_rejected(self):
        for token in (b"/org/bluez/hci0", b"a" * 16, b";whoami"):
            with self.assertRaises(ipc.ProtocolError):
                await self.s.operation(ipc.CONNECT, token)
        self.s.devices["a" * 16] = object()
        self.s.discovered_at = asyncio.get_running_loop().time() - 61
        with self.assertRaises(ipc.ProtocolError):
            await self.s.operation(ipc.CONNECT, b"a" * 16)

    async def test_second_pairing_challenge_rejected(self):
        self.s.devices["a" * 16] = object()
        self.s.discovered_at = asyncio.get_running_loop().time()
        pairing = types.ModuleType("bluez_pair")
        async def pair(device, prompt, register):
            await prompt()
            await prompt()
        pairing.pair_selected = pair
        with patch.dict(sys.modules, {"bluez_pair": pairing}):
            with self.assertRaises(ipc.ProtocolError):
                await self.s.operation(ipc.CONNECT, b"a" * 16)
        self.assertEqual([item[3] for item in self.pipe.sent].count(b"pin"), 1)

    async def test_shared_connect_pair_false_same_selected_object(self):
        device = object()
        self.s.devices["a" * 16] = device
        self.s.discovered_at = asyncio.get_running_loop().time()
        pairing = types.ModuleType("bluez_pair")
        pairing.pair_selected = AsyncMock()
        channel = types.SimpleNamespace(close=AsyncMock())

        async def connect(peer, **kwargs):
            self.assertIs(peer, device)
            self.assertFalse(kwargs["pair"])
            kwargs["progress"]("Subscribing to Bluetooth notifications…")
            return channel

        with patch.dict(sys.modules, {"bluez_pair": pairing}), patch.object(
                bluetooth.BLEChannel, "connect", AsyncMock(side_effect=connect)):
            await self.s.operation(ipc.CONNECT, b"a" * 16)
        self.assertIs(pairing.pair_selected.await_args.args[0], device)
        self.assertEqual(self.s.devices, {})
        self.assertIn(b"subscribing", [item[3] for item in self.pipe.sent])
        self.assertIs(self.s.channel, channel)

    async def test_pin_requires_active_current_session_request(self):
        for envelope in ((ipc.PIN, 8, 1, b"0" * 6), (ipc.PIN, 7, 2, b"0" * 6),
                         (ipc.PIN, 7, 1, b"x" * 6), (ipc.PIN, 7, 1, b"0" * 6)):
            pipe = FakePipe()
            s = helper.Session(pipe)
            s.session, s.request = 7, 1
            pipe.incoming.put_nowait(envelope)
            with self.assertRaises(ipc.ProtocolError):
                await s.run()
            self.assertTrue(pipe.closed)

    async def test_active_pin_and_no_second_response(self):
        entered = asyncio.Event()
        completed = asyncio.Event()
        answers = []

        class Agent:
            def answer(self, value):
                answers.append(len(value))
                completed.set()
                return True

        async def operation(kind, payload):
            self.s.agent = Agent()
            entered.set()
            await completed.wait()
            self.s.agent = None
            return b""

        self.s.session = self.s.request = 0
        self.s.operation = operation
        self.pipe.incoming.put_nowait((ipc.CONNECT, 7, 1, b"a" * 16))
        task = asyncio.create_task(self.s.run())
        await entered.wait()
        self.pipe.incoming.put_nowait((ipc.PIN, 7, 1, b"0" * 6))
        await asyncio.wait_for(completed.wait(), 1)
        await asyncio.sleep(.01)
        self.pipe.incoming.put_nowait((ipc.PIN, 7, 1, b"0" * 6))
        with self.assertRaises(ipc.ProtocolError):
            await task
        self.assertEqual(answers, [6])

    async def test_death_idle_terminates_and_closes(self):
        self.s.channel = types.SimpleNamespace(close=AsyncMock())
        channel = self.s.channel
        task = asyncio.create_task(self.s.run())
        self.s.disconnected(Exception("backend text must not escape"))
        with self.assertRaises(ipc.ProtocolError):
            await task
        channel.close.assert_awaited_once()
        self.assertTrue(self.pipe.closed)

    async def test_error_sanitized_and_no_replay(self):
        self.s.operation = AsyncMock(side_effect=RuntimeError("sensitive backend metadata"))
        self.s.session = self.s.request = 0
        self.pipe.incoming.put_nowait((ipc.CONNECT, 7, 1, b"a" * 16))
        with self.assertRaises(ipc.ProtocolError):
            await self.s.run()
        errors = [item[3] for item in self.pipe.sent if item[0] == ipc.ERROR]
        self.assertEqual(errors, [b"ble_failed_check_dependencies_permissions_pairing"])
        self.s.operation.assert_awaited_once()

    async def test_close_failure_never_acknowledged(self):
        self.s.channel = types.SimpleNamespace(close=AsyncMock(
            side_effect=bluetooth.BluetoothCleanupError("cleanup backend metadata")))
        self.s.session = self.s.request = 0
        self.pipe.incoming.put_nowait((ipc.DISCONNECT, 7, 1, b""))
        with self.assertRaises(ipc.ProtocolError):
            await self.s.run()
        self.assertFalse(any(item[0] == ipc.RESULT for item in self.pipe.sent))
        self.assertEqual([item[3] for item in self.pipe.sent if item[0] == ipc.ERROR],
                         [b"ble_failed_check_dependencies_permissions_pairing"])

    async def test_session_mismatch_and_request_replay_fail_closed(self):
        for envelope in ((ipc.SCAN, 8, 2, b""), (ipc.SCAN, 7, 1, b""), (ipc.RESULT, 7, 2, b"")):
            pipe = FakePipe()
            s = helper.Session(pipe)
            s.session, s.request = 7, 1
            pipe.incoming.put_nowait(envelope)
            with self.assertRaises(ipc.ProtocolError):
                await s.run()

    async def test_slow_write_consumes_single_reply_budget(self):
        async def send(_):
            await asyncio.sleep(.04)
        async def receive(timeout):
            self.assertLess(timeout, .025)
            await asyncio.sleep(1)
        self.s.channel = types.SimpleNamespace(queue=asyncio.Queue(), send=send, receive=receive)
        start = asyncio.get_running_loop().time()
        with self.assertRaises((TimeoutError, RuntimeError)):
            await self.s.operation(ipc.EXCHANGE, struct.pack("!I", 60) + b"\x11\xef" + b"\0" * 8)
        self.assertLess(asyncio.get_running_loop().time() - start, .5)

    async def test_queued_late_reply_never_discarded_to_retry(self):
        queue = asyncio.Queue()
        queue.put_nowait(b"late")
        send = AsyncMock()
        self.s.channel = types.SimpleNamespace(queue=queue, send=send)
        with self.assertRaises(ipc.ProtocolError):
            await self.s.operation(ipc.EXCHANGE, struct.pack("!I", 100) + b"\x11\xef" + b"\0" * 8)
        send.assert_not_awaited()

    async def test_fragmented_and_coalesced_frames(self):
        valid = b"\x11\xef\x03\xe8\x00\x68\x00\x02\x00\x02\x00\xfe"
        for fragments, good in (([valid[:3], valid[3:]], True), ([valid + valid], False),
                                ([b"\x11\xef" + b"\0" * 4 + b"\xff\xff\0"], False)):
            queue = asyncio.Queue()
            values = iter(fragments)
            self.s.channel = types.SimpleNamespace(
                queue=queue, send=AsyncMock(), receive=AsyncMock(side_effect=lambda _: next(values)))
            payload = struct.pack("!I", 100) + b"\x11\xef" + b"\0" * 8
            if good:
                self.assertEqual(await self.s.operation(ipc.EXCHANGE, payload), valid)
            else:
                with self.assertRaises(ipc.ProtocolError):
                    await self.s.operation(ipc.EXCHANGE, payload)


class RuntimeCompatibilityTests(unittest.TestCase):
    def test_installed_bleak_constructor_family_without_client(self):
        from bleak import BleakClient
        version = importlib.metadata.version("bleak")
        if version == "0.22.3":
            self.assertNotIn("pair", inspect.signature(BleakClient).parameters)
        elif version == "3.0.2":
            self.assertIn("pair", inspect.signature(BleakClient).parameters)
        subprocess.run([sys.executable, "-I", "-c",
                        "import sys; sys.path.insert(0, sys.argv[1]); import quadband.bluetooth; "
                        "assert not any(n.startswith(('PySide','PyQt')) for n in sys.modules)",
                        str(ROOT / "desktop/src")], check=True, capture_output=True)


class PipeTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.r, self.writer = os.pipe()
        self.reader, self.w = os.pipe()
        self.pipe = ipc.Pipe(self.r, self.w)

    async def asyncTearDown(self):
        self.pipe.close()
        os.close(self.writer)
        os.close(self.reader)

    async def test_fragmented_and_coalesced_envelopes(self):
        a, b = ipc.encode(ipc.SCAN, 1, 1), ipc.encode(ipc.PREFLIGHT, 1, 2)
        os.write(self.writer, a[:3])
        await asyncio.sleep(.01)
        os.write(self.writer, a[3:] + b)
        self.assertEqual(await self.pipe.receive(), (ipc.SCAN, 1, 1, b""))
        self.assertEqual(await self.pipe.receive(), (ipc.PREFLIGHT, 1, 2, b""))

    async def test_oversize_and_queue_overflow_are_fatal(self):
        for frame in (struct.pack("!I", 8193), ipc.encode(ipc.SCAN, 1, 1) * 5):
            self.pipe.buffer.clear()
            os.write(self.writer, frame)
            with self.assertRaises(ipc.ProtocolError):
                await asyncio.wait_for(self.pipe.receive(), 1)
            # Callback removed on fatal input. Re-arm only within this isolated fixture.
            self.pipe.loop.add_reader(self.r, self.pipe._read)

    async def test_invalid_types_versions_and_lengths(self):
        for args in ((True, 1, 1), (ipc.SCAN, 0, 1), (ipc.SCAN, 1, 0),
                     (ipc.SCAN, 1, 1, b"a" * 8193)):
            with self.assertRaises(ipc.ProtocolError):
                ipc.encode(*args)
        frame = bytearray(ipc.encode(ipc.SCAN, 1, 1))
        frame[4] = 2
        os.write(self.writer, frame)
        with self.assertRaises(ipc.ProtocolError):
            await self.pipe.receive()


class BlueZTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        # Mock only D-Bus bindings; this exercises our actual pairing flow without a bus.
        dbus = types.ModuleType("dbus_fast")
        dbus.BusType = types.SimpleNamespace(SYSTEM=1)
        dbus.DBusError = type("DBusError", (Exception,), {})
        service = types.ModuleType("dbus_fast.service")
        service.ServiceInterface = type("ServiceInterface", (), {"__init__": lambda self, name: None})
        service.method = lambda: lambda fn: fn
        aio = types.ModuleType("dbus_fast.aio")
        aio.MessageBus = object
        self.modules = patch.dict(sys.modules, {"dbus_fast": dbus, "dbus_fast.aio": aio,
                                               "dbus_fast.service": service})
        self.modules.start()
        spec = importlib.util.spec_from_file_location("zero_test_bluez", HELPER / "bluez_pair.py")
        self.bluez = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.bluez)

    async def asyncTearDown(self):
        self.modules.stop()

    async def test_agent_rejects_wrong_peer_and_cancelled_pin(self):
        agent = self.bluez.KeyboardAgent("/selected", AsyncMock())
        self.assertFalse(agent.answer("0" * 6))
        with self.assertRaises(Exception):
            await agent.ask("/other")
        task = asyncio.create_task(agent.ask("/selected"))
        await asyncio.sleep(0)
        agent.Cancel()
        with self.assertRaises(Exception):
            await task
        self.assertIsNone(agent.pending)
        self.assertFalse(agent.answer("0" * 6))

    async def test_same_bus_agent_registration_and_pair_selected_only(self):
        calls = []
        peer_path = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
        class Bus:
            async def connect(bus):
                return bus
            async def introspect(bus, name, path):
                self.assertIn(path, ("/org/bluez", peer_path))
                return None
            def get_proxy_object(bus, name, path, _):
                calls.append(("object", id(bus), path))
                return bus
            def get_interface(bus, name):
                return bus
            async def call_get(bus, iface, name):
                return types.SimpleNamespace(value=False)
            def export(bus, path, agent):
                calls.append(("export", id(bus), path))
            async def call_register_agent(bus, path, capability):
                calls.append(("register", id(bus), capability))
            async def call_pair(bus):
                calls.append(("pair", id(bus)))
            async def call_cancel_pairing(bus):
                calls.append(("cancel", id(bus)))
            async def call_unregister_agent(bus, path):
                calls.append(("unregister", id(bus), path))
            def unexport(bus, path):
                pass
            def disconnect(bus):
                calls.append(("disconnect", id(bus)))
        bus = Bus()
        registered = []
        with patch.object(self.bluez, "MessageBus", return_value=bus):
            await self.bluez.pair_selected(types.SimpleNamespace(details={"path": peer_path}),
                                           AsyncMock(), registered.append)
        register = next(item for item in calls if item[0] == "register")
        pair = next(item for item in calls if item[0] == "pair")
        self.assertEqual(register[1], pair[1])
        self.assertEqual(register[2], "KeyboardOnly")
        self.assertEqual(calls[2][0], "export")
        self.assertIsNone(registered[-1])
        self.assertIn("unregister", [item[0] for item in calls])


if __name__ == "__main__":
    unittest.main()
