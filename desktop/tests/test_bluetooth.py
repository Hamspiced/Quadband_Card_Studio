"""Device-free Bluetooth backend compatibility and fail-closed regressions."""
import asyncio
from types import SimpleNamespace

import pytest
from quadband import bluetooth as ble, transports
from quadband.common import CardError


class Client:
    def __init__(self, device, *, pair=False, disconnected_callback, timeout):
        self.events = [('constructor-pair', pair)]
        self.is_connected = False
        self.disconnected = disconnected_callback
        self.characteristics = {
            ble.NUS_RX: SimpleNamespace(properties=['write', 'write-without-response']),
            ble.NUS_TX: SimpleNamespace(properties=['notify']),
            ble.X6_RX: SimpleNamespace(properties=['write-without-response']),
            ble.X6_TX: SimpleNamespace(properties=['notify']),
        }
        self.services = self
        self.notification_error = self.read_error = None

    def get_characteristic(self, uuid):
        return self.characteristics.get(uuid)

    async def connect(self):
        self.events.append('connect')
        self.is_connected = True

    async def pair(self):
        self.events.append('pair')
        return True

    async def read_gatt_char(self, characteristic):
        self.events.append('battery')
        if self.read_error:
            raise self.read_error
        return b'\x64'

    async def start_notify(self, uuid, callback):
        self.events.append('subscribe')
        self.notify = callback
        if self.notification_error:
            raise self.notification_error

    async def write_gatt_char(self, uuid, data, response):
        self.events.append(('write', data, response))

    async def disconnect(self):
        self.events.append('disconnect')
        self.is_connected = False
        self.disconnected(self)


@pytest.fixture
def backend(monkeypatch):
    import bleak
    created = []
    class Factory(Client):
        def __init__(self, device, *, pair=False, **kwargs):
            super().__init__(device, pair=pair, **kwargs)
            created.append(self)
    monkeypatch.setattr(bleak, 'BleakClient', Factory)
    monkeypatch.setattr(ble.sys, 'platform', 'linux')
    return created


async def test_compatible_reexports_and_modern_pairing(backend):
    assert transports.BLEChannel is ble.BLEChannel
    assert transports.ble_devices is ble.ble_devices
    progress = []
    channel = await ble.BLEChannel.connect(object(), True, progress.append)
    assert backend[0].events == [('constructor-pair', True), 'connect', 'subscribe']
    assert any('Pairing' in p for p in progress)
    assert 'Validating' in progress[-1] and 'ready' not in progress[-1]
    await channel.close()


@pytest.mark.parametrize('pair,expected', [(None, True), (False, False), (True, True)])
async def test_legacy_bleak_requires_explicit_pair(monkeypatch, pair, expected):
    import bleak
    class Legacy(Client):
        def __init__(self, device, **kwargs):
            assert 'pair' not in kwargs
            super().__init__(device, **kwargs)
    monkeypatch.setattr(bleak, 'BleakClient', Legacy)
    monkeypatch.setattr(ble.sys, 'platform', 'linux')
    channel = await ble.BLEChannel.connect(object(), True, pair=pair)
    assert ('pair' in channel.client.events) is expected
    await channel.close()


async def test_macos_does_not_request_explicit_pairing(backend, monkeypatch):
    monkeypatch.setattr(ble.sys, 'platform', 'darwin')
    channel = await ble.BLEChannel.connect(object(), True)
    assert backend[0].events[0] == ('constructor-pair', False)
    await channel.close()


@pytest.mark.parametrize('failure', ['battery', 'notify'])
async def test_authentication_errors_are_not_swallowed_or_disclosed(monkeypatch, failure):
    import bleak
    created = []
    class Protected(Client):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            created.append(self)
            self.characteristics['00002a19-0000-1000-8000-00805f9b34fb'] = SimpleNamespace(properties=['read'])
            error = RuntimeError('org.bluez.Error.AuthenticationRejected sensitive-backend-detail')
            if failure == 'battery':
                self.read_error = error
            else:
                self.notification_error = error
    monkeypatch.setattr(bleak, 'BleakClient', Protected)
    with pytest.raises(CardError, match='pairing') as error:
        await ble.BLEChannel.connect(object(), True, pair=False)
    assert 'sensitive-backend-detail' not in str(error.value)
    assert created[0].events[-1] == 'disconnect'
    if failure == 'battery':
        assert 'subscribe' not in created[0].events


async def test_disconnect_discards_queued_bytes_wakes_reader_and_fires_once(backend):
    failures = []
    channel = await ble.BLEChannel.connect(object(), True, on_disconnect=failures.append)
    client = backend[0]
    client.notify(None, b'stale')
    client.disconnected(client)
    client.disconnected(client)
    client.notify(None, b'late')
    with pytest.raises(CardError, match='disconnected'):
        await channel.receive()
    with pytest.raises(CardError):
        await channel.send(b'not replayed')
    await channel.close()
    assert len(failures) == 1
    assert not any(isinstance(e, tuple) and e[0] == 'write' for e in client.events)
    assert client.events.count('disconnect') == 1


@pytest.mark.parametrize('overflow', ['count', 'bytes'])
async def test_queue_bounds_fail_closed(backend, overflow):
    channel = await ble.BLEChannel.connect(object(), True)
    if overflow == 'count':
        for _ in range(channel.MAX_NOTIFICATIONS + 1):
            backend[0].notify(None, b'x')
    else:
        backend[0].notify(None, bytes(channel.MAX_BYTES + 1))
    with pytest.raises(CardError, match='overflow'):
        await channel.receive()
    assert channel.closed and channel.queue.qsize() == 1
    await channel.close()


async def test_close_unblocks_receive_and_ignores_late_notifications(backend):
    failures = []
    channel = await ble.BLEChannel.connect(object(), True, on_disconnect=failures.append)
    waiting = asyncio.create_task(channel.receive())
    await asyncio.sleep(0)
    with pytest.raises(CardError, match='receive is still running'):
        await channel.receive()
    await channel.close()
    with pytest.raises(CardError, match='closed'):
        await waiting
    backend[0].notify(None, b'late')
    assert failures == []
    await channel.close()


@pytest.mark.parametrize('cancel', [True, False])
async def test_receive_timeout_or_cancel_permanently_closes(backend, cancel):
    channel = await ble.BLEChannel.connect(object(), True)
    task = asyncio.create_task(channel.receive(timeout=.01 if not cancel else None))
    await asyncio.sleep(0)
    if cancel:
        task.cancel()
    with pytest.raises(asyncio.CancelledError if cancel else TimeoutError):
        await task
    assert channel.closed
    assert backend[0].events[-1] == 'disconnect'


async def test_cancel_connection_closes_once(monkeypatch):
    import bleak
    entered, created = asyncio.Event(), []
    class Slow(Client):
        async def start_notify(self, uuid, callback):
            created.append(self)
            entered.set()
            await asyncio.Event().wait()
    monkeypatch.setattr(bleak, 'BleakClient', Slow)
    task = asyncio.create_task(ble.BLEChannel.connect(object(), True, pair=False))
    await entered.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert created[0].events.count('disconnect') == 1


async def test_msr_and_fragmented_notifications_preserved(backend):
    channel = await ble.BLEChannel.connect(object())
    for part in (b'a', b'bc', b'def'):
        backend[0].notify(None, part)
    assert [await channel.receive() for _ in range(3)] == [b'a', b'bc', b'def']
    await channel.send(bytes(41))
    writes = [e for e in backend[0].events if isinstance(e, tuple) and e[0] == 'write']
    assert [len(e[1]) for e in writes] == [20, 20, 1]
    assert all(e[2] is False for e in writes)
    assert backend[0].events[0] == ('constructor-pair', False)
    await channel.close()


@pytest.mark.parametrize('text,expected', [
    ('org.bluez.Error.NotReady', 'adapter'),
    ('Bluetooth permission denied', 'permission'),
    ('AuthenticationFailed', 'pairing'),
    ('unclassified', 'operation failed')])
def test_safe_actionable_backend_errors(text, expected):
    assert expected in str(ble.bluetooth_error(RuntimeError(text)))


async def test_each_scan_is_fresh_and_candidates_are_not_trusted(monkeypatch):
    import bleak
    peers = [SimpleNamespace(name='ChameleonUltra', address=str(i)) for i in range(2)]
    scans = [[peers[0]], [peers[1]], []]
    class Scanner:
        @staticmethod
        async def discover(**kwargs):
            assert kwargs == dict(timeout=2, return_adv=True)
            return {p.address: (p, SimpleNamespace(local_name=None, service_uuids=None)) for p in scans.pop(0)}
    monkeypatch.setattr(bleak, 'BleakScanner', Scanner)
    assert (await ble.ble_devices(True, 2))[0][0] is peers[0]
    assert (await ble.ble_devices(True, 2))[0][0] is peers[1]
    assert await ble.ble_devices(True, 2) == []


@pytest.mark.parametrize('properties,error', [
    ([], 'cannot receive'), (None, 'UART is unavailable')])
async def test_missing_or_unwritable_uart_fails_before_notifications(backend, monkeypatch, properties, error):
    original = Client.connect
    async def connect(self):
        await original(self)
        if properties is None:
            self.characteristics.pop(ble.NUS_RX)
        else:
            self.characteristics[ble.NUS_RX].properties = properties
    monkeypatch.setattr(Client, 'connect', connect)
    with pytest.raises(CardError, match=error):
        await ble.BLEChannel.connect(object(), True)
    assert 'subscribe' not in backend[0].events
    assert backend[0].events[-1] == 'disconnect'


async def test_unconfirmed_cleanup_is_reported_and_prevents_service_transport_reuse(backend, monkeypatch):
    from quadband import chameleon
    from quadband.service import Service
    async def identify(self):
        return 'Mock validated identity'
    async def stuck_disconnect(self):
        self.events.append('disconnect-failed')
        raise RuntimeError('Backend cleanup failed')
    monkeypatch.setattr(chameleon.Chameleon, 'connect', identify)
    monkeypatch.setattr(Client, 'disconnect', stuck_disconnect)
    service = Service(lambda _: None)
    await service.connect('chameleon', 'Bluetooth', object(), 115200)
    backend[0].disconnected(backend[0])
    await asyncio.sleep(0)
    with pytest.raises(ble.BluetoothCleanupError, match='cleanup'):
        await service.disconnect('chameleon')
    with pytest.raises(CardError, match='Restart'):
        await service.connect('chameleon', 'USB serial', 'not-opened', 115200)
    assert len(backend) == 1


async def test_service_idle_loss_and_old_callback_cannot_clear_new_session(backend, monkeypatch):
    from quadband import chameleon
    from quadband.service import Service
    async def identify(self):
        return 'Mock validated identity'
    monkeypatch.setattr(chameleon.Chameleon, 'connect', identify)
    service = Service(lambda _: None)
    changes = []
    service.reader_disconnected = lambda *args: changes.append(args)
    await service.connect('chameleon', 'Bluetooth', object(), 115200)
    first = backend[0]
    first.disconnected(first)
    assert service.chameleon is None
    with pytest.raises(CardError, match='Connect'):
        service.require('chameleon')
    await service.connect('chameleon', 'Bluetooth', object(), 115200)
    second = service.chameleon
    first.disconnected(first)
    assert service.chameleon is second
    assert len(changes) == 1
    await service.disconnect('chameleon')
