"""Exclusive desktop hardware session. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from .common import require
from . import transports, magstripe, chameleon, uhf, pcsc


class Service:
    def __init__(self, progress):
        self.progress = progress
        self.msr = self.chameleon = self.uhf = None

    async def discover(self, mode, family):
        if mode == 'USB HID':
            return transports.hid_devices()
        if mode == 'Bluetooth':
            return await transports.ble_devices(family == 'chameleon')
        return transports.serial_devices()

    async def disconnect(self, family):
        device = getattr(self, family)
        setattr(self, family, None)
        if device:
            if family == 'msr':
                await device.channel.close()
            else:
                await device.close()
        return 'Reader disconnected.'

    async def connect(self, family, mode, device, baud, address=0):
        require(device is not None, 'Refresh and select a reader first.')
        await self.disconnect(family)
        if mode == 'Bluetooth':
            channel = await transports.BLEChannel.connect(device, family == 'chameleon', self.progress)
        elif mode == 'USB HID':
            channel = transports.HIDChannel(device)
        else:
            channel = transports.SerialChannel(device, baud)
        client = None
        try:
            if family == 'msr':
                client = magstripe.MSRClient(channel)
            elif family == 'chameleon':
                connection = transports.FramedConnection(channel, chameleon.Decoder(), chameleon.encode, self.progress)
                client = chameleon.Chameleon(connection)
            else:
                connection = transports.FramedConnection(channel, uhf.Decoder(), lambda cmd, data: uhf.encode(cmd, data, address), self.progress)
                client = uhf.UHF(connection, address)
            label = await client.connect()
            setattr(self, family, client)
            return label
        except BaseException:
            if family != 'msr' and client is not None:
                await client.close()
            else:
                await channel.close()
            raise

    def require(self, family):
        device = getattr(self, family)
        require(device is not None, 'Connect the reader first.')
        return device

    def hf_session(self, name):
        return pcsc.session(name, self.chameleon)

    async def hf_read(self, name, key):
        from .hf import Snapshot
        async with self.hf_session(name) as client:
            info = await client.backend.identify()
            if not info.classic:
                return Snapshot(info)
            return await client.read(key)

    async def close(self):
        for family in ('msr', 'uhf', 'chameleon'):
            try:
                await self.disconnect(family)
            except Exception:
                pass
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
