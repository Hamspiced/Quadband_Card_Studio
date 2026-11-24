"""Exclusive desktop hardware session. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from contextlib import asynccontextmanager
from .common import require
from .bluetooth import BluetoothCleanupError
from . import transports, magstripe, chameleon, uhf, m100, pcsc, cores3


class Service:
    def __init__(self, progress):
        self.progress = progress
        self.msr = self.chameleon = self.uhf = None
        self.bridge = None
        self.bridge_changed = lambda _: None
        self.bridge_leases = {}
        self.reader_disconnected = lambda family, message: None
        self._sessions = {}
        self._unclean_readers = set()
        self._channels = {}

    async def connect_bridge(self, device):
        await self.disconnect_bridge()
        self.bridge = await cores3.Session.connect(device, self.progress, self.bridge_changed)
        return 'CoreS3 connected · authenticated service'

    async def disconnect_bridge(self):
        for family in list(self.bridge_leases):
            try:
                await self.disconnect(family)
            except Exception:
                pass
        if self.bridge:
            bridge, self.bridge = self.bridge, None
            await bridge.close()
        self.bridge_changed(None)
        return 'CoreS3 disconnected.'

    async def bridge_reader(self, family):
        require(self.bridge is not None and not self.bridge.closed, 'Connect CoreS3 in the sidebar first.')
        number, capability = (2, 'm100') if family == 'uhf' else (3, 'chameleon')
        slot = self.bridge.slots.get(number, {})
        require(slot.get('present') and capability in slot.get('capabilities', []), 'CoreS3 endpoint unavailable.')
        await self.disconnect(family)
        lease = self.bridge.lease(number)
        endpoint = await lease.__aenter__()
        self.bridge_leases[family] = lease
        try:
            decoder, encode = (m100.Decoder(), m100.encode) if family == 'uhf' else (chameleon.Decoder(), chameleon.encode)
            transport = cores3.RawConnection(endpoint, decoder, encode)
            client = m100.M100(transport, 'M5Stack JRD-4035') if family == 'uhf' else chameleon.Chameleon(transport)
            setattr(self, family, client)
            return await client.connect()
        except BaseException:
            await self.disconnect(family)
            raise

    async def discover(self, mode, family):
        if mode == 'CoreS3':
            require(self.bridge is not None and not self.bridge.closed, 'Connect CoreS3 in the sidebar first.')
            number = 2 if family == 'uhf' else 3
            slot = self.bridge.slots[number]
            return [(number, f"CoreS3 · {slot['kind']}")] if slot['present'] else []
        if mode == 'USB HID':
            return transports.hid_devices()
        if mode == 'Bluetooth':
            return await transports.ble_devices(family == 'chameleon')
        return transports.serial_devices()

    async def disconnect(self, family):
        require(family not in self._unclean_readers,
                'Reader cleanup could not be confirmed. Restart the application before reconnecting.')
        self._sessions.pop(family, None)
        device = getattr(self, family)
        channel = self._channels.pop(family, None)
        setattr(self, family, None)
        try:
            if device:
                if family == 'msr':
                    await device.channel.close()
                else:
                    await device.close()
            elif channel:
                await channel.close()
        except BluetoothCleanupError:
            self._unclean_readers.add(family)
            raise
        finally:
            lease = self.bridge_leases.pop(family, None)
            if lease:
                await lease.__aexit__(None, None, None)
            if device:
                self.reader_disconnected(family, 'Reader disconnected.')
        return 'Reader disconnected.'

    async def connect(self, family, mode, device, baud, address=0, reader_profile='Piswords UHF2'):
        require(device is not None, 'Refresh and select a reader first.')
        if mode == 'CoreS3':
            require(family in ('uhf', 'chameleon'), 'Unsupported bridge reader.')
            return await self.bridge_reader(family)
        if family == 'uhf':
            require(mode == 'USB serial', 'UHF modules require a USB-serial connection.')
            require(reader_profile in ('Piswords UHF2', 'M5Stack JRD-4035', 'YRM100'), 'Choose a supported UHF reader profile.')
        await self.disconnect(family)
        token = object()
        self._sessions[family] = token
        def lost(error):
            if self._sessions.get(family) is token:
                self._sessions.pop(family, None)
                setattr(self, family, None)
                self.reader_disconnected(family, str(error))
        if mode == 'Bluetooth':
            try:
                channel = await transports.BLEChannel.connect(
                    device, family == 'chameleon', self.progress, on_disconnect=lost)
            except BluetoothCleanupError:
                self._unclean_readers.add(family)
                raise
        elif mode == 'USB HID':
            channel = transports.HIDChannel(device)
        else:
            channel = transports.SerialChannel(device, baud)
        self._channels[family] = channel
        client = None
        try:
            if family == 'msr':
                client = magstripe.MSRClient(channel)
            elif family == 'chameleon':
                connection = transports.FramedConnection(channel, chameleon.Decoder(), chameleon.encode, self.progress)
                client = chameleon.Chameleon(connection)
            elif reader_profile != 'Piswords UHF2':
                connection = transports.FramedConnection(channel, m100.Decoder(), m100.encode, self.progress)
                client = m100.M100(connection, reader_profile)
            else:
                connection = transports.FramedConnection(channel, uhf.Decoder(), lambda cmd, data: uhf.encode(cmd, data, address), self.progress)
                client = uhf.UHF(connection, address)
            if family != 'msr':
                connection.on_disconnect = lost
            label = await client.connect()
            require(self._sessions.get(family) is token, 'Reader disconnected while validating. Scan and reconnect.')
            setattr(self, family, client)
            return label
        except BaseException:
            try:
                if family != 'msr' and client is not None:
                    await client.close()
                else:
                    await channel.close()
            except BluetoothCleanupError:
                self._unclean_readers.add(family)
                raise
            raise

    def require(self, family):
        device = getattr(self, family)
        require(device is not None, 'Connect the reader first.')
        transport = getattr(device, 'transport', None)
        channel = getattr(device, 'channel', None)
        require(not getattr(transport, 'closed', False) and not getattr(channel, 'closed', False),
                'Reader disconnected. Reconnect it.')
        return device

    @asynccontextmanager
    async def hf_session(self, name, authentication=b'\0'):
        if name == cores3.HF_READER:
            require(self.bridge is not None and not self.bridge.closed, 'Connect CoreS3 first.')
            async with self.bridge.lease(1) as endpoint:
                backend = cores3.HF(endpoint, authentication)
                try:
                    yield cores3.HFClient(backend)
                finally:
                    try:
                        if not endpoint.session.closed and endpoint.session.leases.get(1) is endpoint.token:
                            await backend.reset_authentication()
                    finally:
                        await backend.close()
        else:
            async with pcsc.session(name, self.chameleon) as client:
                yield client

    async def hf_read(self, name, key, authentication=b'\0'):
        from .hf import Snapshot
        async with self.hf_session(name, authentication) as client:
            info = await client.backend.identify()
            if not info.classic and not info.ntag:
                return Snapshot(info)
            return await client.read(key)

    async def close(self):
        for family in ('msr', 'uhf', 'chameleon'):
            try:
                await self.disconnect(family)
            except Exception:
                pass
        await self.disconnect_bridge()
        return 'Connections closed.'


class StopFlag:
    """threading.Event is safe to set from Qt and inspect from the asyncio worker."""
    def __init__(self):
        import threading
        self.event = threading.Event()

    def is_set(self):
        return self.event.is_set()

    def set(self):
        self.event.set()


async def call_sync(function):
    await asyncio.sleep(0)
    return function()
