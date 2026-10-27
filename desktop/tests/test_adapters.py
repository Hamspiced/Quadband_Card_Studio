import asyncio
import pytest
from quadband import pcsc, transports, hf
from quadband.common import NoCard


class FakeConnection:
    def __init__(self):
        self.calls, self.absent = [], False
    def connect(self, *args, **kwargs):
        from smartcard.Exceptions import NoCardException
        self.calls.append('connect')
        if self.absent:
            raise NoCardException('No card', 0x8010000C)
    def lock(self):
        self.calls.append('lock')
    def unlock(self):
        self.calls.append('unlock')
    def disconnect(self):
        self.calls.append('disconnect')
    def reconnect(self, **kwargs):
        self.calls.append('reset')
    def transmit(self, command):
        self.calls.append(command)
        if command[1] == 0xCA:
            return [1, 2, 3, 4], 0x90, 0
        return ([0]*16 if command[1] == 0xB0 else []), 0x90, 0
    def getATR(self):
        return list(bytes.fromhex('3B8F8001804F0CA000000306030001000000006A'))


@pytest.fixture
def connection(monkeypatch):
    import smartcard.ExclusiveTransmitCardConnection as exclusive
    fake = FakeConnection()
    class Reader:
        def createConnection(self):
            return fake
    monkeypatch.setattr(pcsc, 'find_reader', lambda _: Reader())
    monkeypatch.setattr(exclusive, 'ExclusiveTransmitCardConnection', lambda c: c)
    return fake


async def test_pcsc_transaction_authentication_and_cleanup(connection):
    async with pcsc.session('test') as client:
        card = await client.backend.identify()
        assert card.classic
        assert await client.backend.read_block(4, hf.FACTORY) == bytes(16)
        await client.backend.reset_authentication()
    assert connection.calls[:2] == ['connect', 'lock']
    assert [255, 0x82, 0, 0, 6, *([255]*6)] in connection.calls
    assert [255, 0x86, 0, 0, 5, 1, 0, 4, 0x60, 0] in connection.calls
    assert connection.calls[-2:] == ['unlock', 'disconnect']
    assert connection.calls.count('lock') == 2


async def test_pcsc_no_card_is_presence_false(connection):
    connection.absent = True
    backend = pcsc.PCSC('test')
    assert not await backend.present()
    with pytest.raises(NoCard):
        await backend.identify()
    assert 'lock' not in connection.calls


async def test_ble_chameleon_pairs_then_protected_read_then_notifications(monkeypatch):
    import bleak
    calls = []
    class Characteristic:
        properties = ['read', 'write']
    class Services:
        def get_characteristic(self, _):
            return Characteristic()
    class Client:
        def __init__(self, device, **kwargs):
            calls.append(('construct', kwargs['pair']))
            self.is_connected = False
            self.services = Services()
        async def connect(self):
            self.is_connected = True
            calls.append('connect')
        async def read_gatt_char(self, _):
            calls.append('protected-read')
            return b'\x64'
        async def start_notify(self, _, callback):
            calls.append('subscribe')
            callback(None, b'reply')
        async def write_gatt_char(self, _, data, response):
            calls.append(('write', data, response))
        async def disconnect(self):
            self.is_connected = False
            calls.append('disconnect')
    monkeypatch.setattr(bleak, 'BleakClient', Client)
    monkeypatch.setattr(transports.sys, 'platform', 'win32')
    channel = await transports.BLEChannel.connect(object(), True)
    assert calls[:4] == [('construct', True), 'connect', 'protected-read', 'subscribe']
    assert await channel.receive() == b'reply'
    await channel.send(bytes(41))
    assert [len(c[1]) for c in calls if isinstance(c, tuple) and c[0] == 'write'] == [20, 20, 1]
    await channel.close()


async def test_ble_connection_failure_closes_client(monkeypatch):
    import bleak
    calls = []
    class Client:
        def __init__(self, *args, **kwargs):
            self.is_connected = True
        async def connect(self):
            raise RuntimeError('Pairing failed')
        async def disconnect(self):
            self.is_connected = False
            calls.append('closed')
    monkeypatch.setattr(bleak, 'BleakClient', Client)
    with pytest.raises(RuntimeError, match='Pairing'):
        await transports.BLEChannel.connect(object(), True)
    assert calls == ['closed']


async def test_real_serial_adapter_over_loopback(monkeypatch):
    import serial
    monkeypatch.setattr(serial, 'Serial', lambda path, **kwargs: serial.serial_for_url('loop://', **kwargs))
    channel = transports.SerialChannel('test', 9600)
    await channel.send(b'fragment-test')
    assert await asyncio.wait_for(channel.receive(), 1) == b'fragment-test'
    await channel.close()
