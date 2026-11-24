"""Ephemeral keyboard agent restricted to the explicitly selected BlueZ device."""
# D-Bus method annotations are wire signatures rather than Python forward references.
# ruff: noqa: F821
import asyncio
import re
from dbus_fast import BusType, DBusError
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, method

NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"


class KeyboardAgent(ServiceInterface):
    def __init__(self, peer, prompt):
        super().__init__("org.bluez.Agent1")
        self.peer, self.prompt = peer, prompt
        self.pending = None

    def selected(self, device):
        if device != self.peer:
            raise DBusError("org.bluez.Error.Rejected", "Unselected device")

    async def ask(self, device):
        self.selected(device)
        if self.pending is not None:
            raise DBusError("org.bluez.Error.Rejected", "Prompt already active")
        self.pending = asyncio.get_running_loop().create_future()
        try:
            await self.prompt()
            value = await asyncio.wait_for(self.pending, 30)
            if not isinstance(value, str) or not value.isascii() or not value.isdigit() or len(value) != 6:
                raise DBusError("org.bluez.Error.Rejected", "Invalid PIN")
            return value
        except (asyncio.TimeoutError, asyncio.CancelledError):
            raise DBusError("org.bluez.Error.Canceled", "Pairing canceled") from None
        finally:
            self.pending = None

    def answer(self, value):
        if self.pending is None or self.pending.done():
            return False
        self.pending.set_result(value)
        return True

    @method()
    async def RequestPasskey(self, device: 'o') -> 'u':
        return int(await self.ask(device))

    @method()
    async def RequestPinCode(self, device: 'o') -> 's':
        return await self.ask(device)

    @method()
    def RequestConfirmation(self, device: 'o', passkey: 'u'):
        self.selected(device)
        raise DBusError("org.bluez.Error.Rejected", "Keyboard input required")

    @method()
    def RequestAuthorization(self, device: 'o'):
        self.selected(device)
        raise DBusError("org.bluez.Error.Rejected", "Explicit pairing required")

    @method()
    def AuthorizeService(self, device: 'o', uuid: 's'):
        self.selected(device)
        if uuid.lower() != NUS:
            raise DBusError("org.bluez.Error.Rejected", "Unsupported service")

    @method()
    def Cancel(self):
        if self.pending and not self.pending.done():
            self.pending.cancel()

    @method()
    def Release(self):
        self.Cancel()


async def pair_selected(device, prompt, register):
    path = getattr(device, "details", {}).get("path", "")
    if not isinstance(path, str) or not re.fullmatch(
            r"/org/bluez/hci[0-9]+/dev_[0-9A-Fa-f_]{17}", path):
        raise ValueError("Selected discovery object lacks a BlueZ device path")
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    agent_path = "/com/quadband/zero/keyboard"
    agent = KeyboardAgent(path, prompt)
    manager = peer = None
    registered = False
    try:
        info = await bus.introspect("org.bluez", "/org/bluez")
        manager = bus.get_proxy_object("org.bluez", "/org/bluez", info).get_interface("org.bluez.AgentManager1")
        info = await bus.introspect("org.bluez", path)
        obj = bus.get_proxy_object("org.bluez", path, info)
        properties = obj.get_interface("org.freedesktop.DBus.Properties")
        paired = await properties.call_get("org.bluez.Device1", "Paired")
        if paired.value:
            return
        peer = obj.get_interface("org.bluez.Device1")
        bus.export(agent_path, agent)
        await manager.call_register_agent(agent_path, "KeyboardOnly")
        registered = True
        register(agent)
        # Never RequestDefaultAgent, set Trusted, remove bonds, or alter adapter policy.
        await asyncio.wait_for(peer.call_pair(), 40)
    finally:
        register(None)
        agent.Cancel()
        if peer is not None:
            try:
                await asyncio.wait_for(peer.call_cancel_pairing(), 2)
            except Exception:
                pass
        if registered:
            try:
                await asyncio.wait_for(manager.call_unregister_agent(agent_path), 2)
            except Exception:
                pass
        bus.unexport(agent_path)
        bus.disconnect()
