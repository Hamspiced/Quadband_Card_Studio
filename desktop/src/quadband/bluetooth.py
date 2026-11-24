"""Qt-free BLE byte channels. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
import inspect
import sys

from .common import CardError, require

NUS = '6e400001-b5a3-f393-e0a9-e50e24dcca9e'
NUS_RX = NUS.replace('0001-', '0002-')
NUS_TX = NUS.replace('0001-', '0003-')
X6 = '0000ff00-0000-1000-8000-00805f9b34fb'
X6_RX = X6.replace('ff00-', 'ff02-')
X6_TX = X6.replace('ff00-', 'ff01-')


class BluetoothCleanupError(CardError):
    """The backend has not confirmed link teardown; do not replace the link."""


def bluetooth_error(error):
    """Classify backend failures without exposing backend text or pairing data."""
    if isinstance(error, CardError):
        return error
    text = str(error).lower()
    if any(word in text for word in ('permission', 'notauthorized', 'accessdenied', 'access denied')):
        return CardError('Bluetooth permission denied. Allow this application Bluetooth access in system settings.')
    if any(word in text for word in ('auth', 'pair', 'encrypt', 'security', 'pin', 'passkey')):
        return CardError('Bluetooth pairing failed or was refused. Complete the system pairing prompt '
                         'for the selected reader, then scan and reconnect. No security settings were changed.')
    if any(word in text for word in ('powered', 'adapter', 'notready', 'not ready', 'not available')):
        return CardError('Bluetooth adapter unavailable or powered off. Enable Bluetooth and scan again.')
    if isinstance(error, TimeoutError):
        return CardError('Bluetooth timed out. Keep the reader awake and complete the system pairing prompt; '
                         'scan and reconnect before retrying.')
    return CardError('Bluetooth operation failed. Check the selected reader, system Bluetooth settings and '
                     'other reader apps, then scan and reconnect.')


async def ble_devices(chameleon=False, timeout=6):
    """Return only this bounded scan's candidate peers, never a trusted identity."""
    from bleak import BleakScanner
    require(0 < timeout <= 30, 'Bluetooth scan timeout must be between 0 and 30 seconds.')
    try:
        found = await asyncio.wait_for(
            BleakScanner.discover(timeout=timeout, return_adv=True), timeout + 2)
    except Exception as exc:
        raise bluetooth_error(exc) from None
    service = NUS if chameleon else X6
    devices = []
    for device, adv in found.values():
        name = adv.local_name or device.name or ''
        if (service in [s.lower() for s in (adv.service_uuids or [])]
                or ('chameleon' if chameleon else 'msr') in name.lower()
                or (not chameleon and 'x6' in name.lower())):
            devices.append((device, f'{name or "Reader"} · {device.address}'))
    return devices


class BLEChannel:
    """One selected link; overflow, timeout and disconnect invalidate all bytes.

    Supports Bleak 0.22.3 (explicit pair()) and 3.0.2 (constructor pair).
    A channel being subscribed is not protocol identity/capability validation.
    """
    framed_hid = False
    MAX_NOTIFICATIONS = 256
    MAX_BYTES = 65536

    def __init__(self):
        self.queue = asyncio.Queue(maxsize=self.MAX_NOTIFICATIONS)
        self.client = None
        self.closed = False
        self.error = None
        self._queued_bytes = 0
        self._on_disconnect = None
        self._cleanup = None
        self._cleanup_error = None
        self._sending = False
        self._receiving = False
        self._loop = asyncio.get_running_loop()

    @classmethod
    async def connect(cls, device, chameleon=False, progress=None, pair=None, on_disconnect=None):
        from bleak import BleakClient
        require(device is not None, 'Scan and select a Bluetooth reader first.')
        progress = progress or (lambda _: None)
        self = cls()
        self._on_disconnect = on_disconnect
        self.rx = NUS_RX if chameleon else X6_RX
        tx = NUS_TX if chameleon else X6_TX
        should_pair = chameleon and sys.platform != 'darwin' if pair is None else pair
        # Older Bleak accepts arbitrary kwargs but does not implement constructor pairing.
        constructor_pair = 'pair' in inspect.signature(BleakClient).parameters
        options = dict(disconnected_callback=lambda _: self._dispatch(
            self._fail, CardError('Bluetooth disconnected. Scan and reconnect before trying again.')), timeout=90)
        if constructor_pair:
            options['pair'] = should_pair
        try:
            self.client = BleakClient(device, **options)
            async with asyncio.timeout(90):
                progress('Connecting Bluetooth…')
                if should_pair:
                    progress('Pairing Bluetooth; complete the system prompt for the selected reader.')
                await self.client.connect()
                self._check_open()
                if should_pair and not constructor_pair:
                    paired = await self.client.pair()
                    require(paired is not False, 'Bluetooth pairing was refused. Scan and reconnect.')
                self._check_open()
                if chameleon:
                    battery = self.client.services.get_characteristic('00002a19-0000-1000-8000-00805f9b34fb')
                    if battery and 'read' in battery.properties:
                        progress('Checking protected Bluetooth access; complete the system prompt if shown.')
                        await self.client.read_gatt_char(battery)
                self._check_open()
                characteristic = self.client.services.get_characteristic(self.rx)
                reader = self.client.services.get_characteristic(tx)
                require(characteristic is not None, 'The expected Bluetooth UART is unavailable.')
                require(reader is not None and any(p in reader.properties for p in ('notify', 'indicate')),
                        'Bluetooth UART notifications are unavailable.')
                self.response = 'write' in characteristic.properties
                require(self.response or 'write-without-response' in characteristic.properties,
                        'This Bluetooth characteristic cannot receive commands.')
                self.chunk = 20
                progress('Subscribing to Bluetooth notifications…')
                await self.client.start_notify(tx, lambda _, data: self._dispatch(self._notify, bytes(data)))
                self._check_open()
                progress('Validating reader identity and capabilities…')
                return self
        except BaseException as exc:
            await self.close()
            if isinstance(exc, Exception):
                raise bluetooth_error(exc) from None
            raise

    def _dispatch(self, callback, value):
        if not self._loop.is_closed():
            try:
                local = asyncio.get_running_loop() is self._loop
            except RuntimeError:
                local = False
            if local:
                callback(value)
            else:
                self._loop.call_soon_threadsafe(callback, value)

    def _check_open(self):
        if self.error:
            raise self.error
        require(not self.closed and self.client and self.client.is_connected, 'Bluetooth is disconnected.')

    def _notify(self, data):
        if self.closed or not data:
            return
        if self.queue.full() or self._queued_bytes + len(data) > self.MAX_BYTES:
            self._fail(CardError('Bluetooth receive queue overflowed. Scan and reconnect; no commands were replayed.'))
            return
        self._queued_bytes += len(data)
        self.queue.put_nowait(data)

    def _fail(self, error, notify=True):
        if self.closed:
            return
        self.closed = True
        self.error = error
        while not self.queue.empty():
            self.queue.get_nowait()
        self._queued_bytes = 0
        self.queue.put_nowait(error)
        self._start_cleanup()
        if notify and self._on_disconnect:
            self._on_disconnect(error)

    def _start_cleanup(self):
        if self._cleanup is None:
            self._cleanup = self._loop.create_task(self._disconnect())

    async def _disconnect(self):
        if self.client:
            try:
                await asyncio.wait_for(self.client.disconnect(), 5)
                if self.client.is_connected:
                    raise RuntimeError('Link remains connected')
            except Exception:
                self._cleanup_error = BluetoothCleanupError(
                    'Bluetooth cleanup could not be confirmed. Restart the application before changing transport.')

    async def send(self, message):
        self._check_open()
        require(not self._sending, 'Another Bluetooth transmission is still running.')
        self._sending = True
        try:
            for i in range(0, len(message), self.chunk):
                self._check_open()
                await asyncio.wait_for(
                    self.client.write_gatt_char(self.rx, message[i:i+self.chunk], response=self.response), 10)
                self._check_open()
                if not self.response:
                    await asyncio.sleep(.015)
        except BaseException as exc:
            self._fail(bluetooth_error(exc))
            await self.close()
            if isinstance(exc, Exception):
                raise self.error from None
            raise
        finally:
            self._sending = False

    async def receive(self, timeout=None):
        if self.error:
            raise self.error
        require(not self._receiving, 'Another Bluetooth receive is still running.')
        self._receiving = True
        try:
            try:
                value = await asyncio.wait_for(self.queue.get(), timeout) if timeout is not None else await self.queue.get()
            except (TimeoutError, asyncio.CancelledError):
                await self.close()
                raise
        finally:
            self._receiving = False
        if isinstance(value, Exception):
            raise value
        self._queued_bytes -= len(value)
        return value

    async def close(self):
        if not self.closed:
            self._fail(CardError('Bluetooth connection closed. Scan and reconnect before trying again.'), notify=False)
        self._start_cleanup()
        await asyncio.shield(self._cleanup)
        if self._cleanup_error:
            raise self._cleanup_error
