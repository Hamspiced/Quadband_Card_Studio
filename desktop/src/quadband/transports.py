"""Windows/Linux USB, serial and Bluetooth. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
import sys
from .common import CardError, require
from .magstripe import hid_packets

NUS = '6e400001-b5a3-f393-e0a9-e50e24dcca9e'
NUS_RX = NUS.replace('0001-', '0002-')
NUS_TX = NUS.replace('0001-', '0003-')
X6 = '0000ff00-0000-1000-8000-00805f9b34fb'
X6_RX = X6.replace('ff00-', 'ff02-')
X6_TX = X6.replace('ff00-', 'ff01-')


def serial_devices():
    from serial.tools.list_ports import comports
    return [(p.device, f'{p.description} · {p.device}') for p in comports()]


def hid_devices():
    import hid
    return [(d['path'], f"{d.get('product_string') or 'MSR writer'} · {d['vendor_id']:04X}:{d['product_id']:04X}")
            for d in hid.enumerate() if not (d.get('usage_page') == 1 and d.get('usage') == 6)
            and (d['vendor_id'] == 0x0801 or 'msr' in str(d.get('product_string', '')).lower())]


async def ble_devices(chameleon=False):
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=6, return_adv=True)
    service = NUS if chameleon else X6
    return [(device, f'{adv.local_name or device.name or "Reader"} · {device.address}')
            for device, adv in found.values()
            if service in [s.lower() for s in adv.service_uuids]
            or ('chameleon' if chameleon else 'msr') in (adv.local_name or device.name or '').lower()
            or (not chameleon and 'x6' in (adv.local_name or device.name or '').lower())]


class SerialChannel:
    framed_hid = False

    def __init__(self, path, baud):
        import serial
        self.port = serial.Serial(path, baudrate=baud, timeout=.03, write_timeout=2,
                                  rtscts=False, dsrdtr=False, xonxoff=False)
        self.port.reset_input_buffer()

    async def send(self, message):
        require(self.port.is_open, 'The serial reader is disconnected. Reconnect it.')
        written = self.port.write(message)
        require(written == len(message), 'The serial command was only partly sent. Reconnect.')
        await asyncio.sleep(0)

    async def receive(self):
        while self.port.is_open:
            chunk = self.port.read(max(1, min(self.port.in_waiting, 4096)))
            await asyncio.sleep(.001)
            if chunk:
                return chunk
        raise CardError('The serial reader is disconnected.')

    async def close(self):
        self.port.close()


class HIDChannel:
    framed_hid = True

    def __init__(self, path):
        import hid
        self.device = hid.device()
        self.device.open_path(path)
        self.open = True

    async def send(self, message):
        require(self.open, 'The USB writer is disconnected. Reconnect it.')
        for packet in hid_packets(message):
            # hidapi requires the report ID byte even for unnumbered feature reports.
            require(self.device.send_feature_report(b'\0' + packet) == 65, 'USB feature report failed.')
            await asyncio.sleep(0)

    async def receive(self):
        while self.open:
            data = bytes(self.device.read(64, 30))
            await asyncio.sleep(.001)
            if data:
                return data
        raise CardError('USB writer disconnected.')

    async def close(self):
        if self.open:
            self.open = False
            self.device.close()


class BLEChannel:
    framed_hid = False

    def __init__(self):
        self.queue = asyncio.Queue()
        self.client = None

    @classmethod
    async def connect(cls, device, chameleon=False, progress=lambda _: None):
        from bleak import BleakClient
        self = cls()
        def disconnected(_):
            self.queue.put_nowait(CardError('Bluetooth disconnected. Reconnect before trying again.'))
        self.client = BleakClient(device, disconnected_callback=disconnected,
                                 pair=chameleon and sys.platform != 'darwin', timeout=90)
        self.rx = NUS_RX if chameleon else X6_RX
        tx = NUS_TX if chameleon else X6_TX
        try:
            progress('Connecting Bluetooth; complete a system PIN prompt if it appears.')
            await self.client.connect()
            if chameleon:
                battery = self.client.services.get_characteristic('00002a19-0000-1000-8000-00805f9b34fb')
                if battery and 'read' in battery.properties:
                    progress('Checking the protected Bluetooth connection…')
                    await self.client.read_gatt_char(battery)
            characteristic = self.client.services.get_characteristic(self.rx)
            require(characteristic is not None, 'The expected Bluetooth UART is unavailable.')
            self.response = 'write' in characteristic.properties
            require(self.response or 'write-without-response' in characteristic.properties,
                    'This Bluetooth characteristic cannot receive commands.')
            self.chunk = 20  # Conservative across negotiated MTUs and both device families.
            await self.client.start_notify(tx, lambda _, data: self.queue.put_nowait(bytes(data)))
            return self
        except BaseException:
            await self.close()
            raise

    async def send(self, message):
        require(self.client and self.client.is_connected, 'Bluetooth is disconnected.')
        for i in range(0, len(message), self.chunk):
            await self.client.write_gatt_char(self.rx, message[i:i+self.chunk], response=self.response)
            if not self.response:
                await asyncio.sleep(.015)

    async def receive(self):
        value = await self.queue.get()
        if isinstance(value, Exception):
            raise value
        return value

    async def close(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()


class FramedConnection:
    """One outstanding exchange, with permanent close on timeout/protocol error.

    A late acknowledgement must never satisfy a subsequent write.
    """
    def __init__(self, channel, decoder, encode, progress=None):
        self.channel, self.decoder, self.encode = channel, decoder, encode
        self.progress = progress or (lambda _: None)
        self.pending = None
        self.on_frame = lambda frame: None
        self.closed = False
        self.reader = asyncio.create_task(self._read())

    async def _read(self):
        try:
            while True:
                for frame in self.decoder.feed(await self.channel.receive()):
                    consumed = self.on_frame(frame)
                    if consumed:
                        continue
                    if self.pending and not self.pending[1].done():
                        command, future = self.pending
                        require(frame.command == command, 'Unexpected reader response. Reconnect.')
                        future.set_result(frame)
        except asyncio.CancelledError:
            return
        except Exception as exc:
            self.closed = True
            if self.pending and not self.pending[1].done():
                self.pending[1].set_exception(exc)
            await self.channel.close()
            self.progress(str(exc))

    async def exchange(self, command, data=b'', timeout=3):
        require(not self.closed, 'Reader disconnected. Reconnect it.')
        require(self.pending is None, 'Another reader command is still running.')
        future = asyncio.get_running_loop().create_future()
        self.pending = (command, future)
        try:
            await self.channel.send(self.encode(command, data))
            return await asyncio.wait_for(future, timeout)
        except BaseException:
            await self.close()
            raise
        finally:
            self.pending = None

    async def close(self):
        self.closed = True
        self.reader.cancel()
        if self.pending and not self.pending[1].done():
            self.pending[1].cancel()
        await self.channel.close()
