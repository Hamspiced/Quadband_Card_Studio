"""Windows/Linux USB, serial and Bluetooth. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from .common import CardError, require
from .magstripe import hid_packets
from .bluetooth import BLEChannel as BLEChannel, ble_devices as ble_devices
from .bluetooth import NUS as NUS, NUS_RX as NUS_RX, NUS_TX as NUS_TX
from .bluetooth import X6 as X6, X6_RX as X6_RX, X6_TX as X6_TX


def serial_devices():
    from serial.tools.list_ports import comports
    return [(p.device, f'{p.description} · {p.device}') for p in comports()]


def hid_devices():
    import hid
    return [(d['path'], f"{d.get('product_string') or 'MSR writer'} · {d['vendor_id']:04X}:{d['product_id']:04X}")
            for d in hid.enumerate() if not (d.get('usage_page') == 1 and d.get('usage') == 6)
            and (d['vendor_id'] == 0x0801 or 'msr' in str(d.get('product_string', '')).lower())]


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


class FramedConnection:
    """One outstanding exchange, with permanent close on timeout/protocol error.

    A late acknowledgement must never satisfy a subsequent write.
    """
    def __init__(self, channel, decoder, encode, progress=None):
        self.channel, self.decoder, self.encode = channel, decoder, encode
        self.progress = progress or (lambda _: None)
        self.pending = None
        self.on_frame = lambda frame: None
        self.on_disconnect = lambda error: None
        self.closed = False
        self.reader = asyncio.create_task(self._read())

    async def _read(self):
        try:
            while True:
                for frame in self.decoder.feed(await self.channel.receive()):
                    consumed = self.on_frame(frame)
                    if consumed:
                        continue
                    require(self.pending is not None and not self.pending[1].done(),
                            'Unexpected reader response. Reconnect.')
                    if self.pending and not self.pending[1].done():
                        command, future = self.pending
                        require(frame.command == command, 'Unexpected reader response. Reconnect.')
                        future.set_result(frame)
        except asyncio.CancelledError:
            return
        except Exception as exc:
            was_closed = self.closed
            self.closed = True
            if not was_closed:
                self.on_disconnect(exc)
            if self.pending and not self.pending[1].done():
                self.pending[1].set_exception(exc)
            try:
                await self.channel.close()
            except Exception as cleanup:
                self.progress(str(cleanup))
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
        was_closed = self.closed
        self.closed = True
        if not was_closed:
            self.on_disconnect(CardError('Reader connection closed. Reconnect before trying again.'))
        self.reader.cancel()
        if self.pending and not self.pending[1].done():
            self.pending[1].cancel()
        await self.channel.close()
