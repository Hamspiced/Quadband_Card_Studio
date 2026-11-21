import asyncio
import copy
import json
from pathlib import Path
import struct
from types import SimpleNamespace
import sys

import pytest
from quadband import cores3, hf, m100, chameleon
from quadband.common import CardError, NoCard, AuthenticationError
from quadband.service import Service


ROOT = Path(__file__).resolve().parents[2]
PROTOCOL = ROOT / 'protocol'
if not PROTOCOL.exists():
    PROTOCOL = ROOT / '.build/cores3-tests/protocol'
GOLDEN = json.loads((PROTOCOL / 'fixtures/cores3-v1.json').read_text())


@pytest.mark.parametrize('fixture', GOLDEN['frames'], ids=lambda f: f['name'])
def test_golden_frames_fragmentation_and_coalescing(fixture):
    expected = cores3.Frame(fixture['kind'], int(fixture['session']), fixture['requestId'],
                           fixture['channel'], fixture['operation'], fixture['generation'],
                           fixture['status'], bytes.fromhex(fixture['payloadHex']))
    wire = bytes.fromhex(fixture['frameHex'])
    assert expected.encode() == wire
    for size in (1, 2, 7, 20, 26, 512, 2078):
        decoder, frames = cores3.Decoder(), []
        for offset in range(0, len(wire), size):
            frames += decoder.feed(wire[offset:offset+size])
        assert frames == [expected] and decoder.data == b''
    assert cores3.Decoder().feed(wire + wire) == [expected, expected]


@pytest.mark.parametrize('mutation', ['magic', 'version', 'kind', 'length', 'crc', 'buffer'])
def test_codec_rejects_without_resynchronizing(mutation):
    packet = bytearray(bytes.fromhex(GOLDEN['frames'][0]['frameHex']))
    if mutation == 'magic':
        packet[0] ^= 1
    elif mutation == 'version':
        packet[2] = 2
    elif mutation == 'kind':
        packet[3] = 9
    elif mutation == 'length':
        packet[24:26] = struct.pack('<H', 2049)
    elif mutation == 'crc':
        packet[-1] ^= 1
    else:
        packet = packet * 300
    decoder = cores3.Decoder()
    with pytest.raises(CardError):
        decoder.feed(packet)
    assert not decoder.data


def test_manifest_uuids_sizes_capabilities():
    manifest = json.loads((PROTOCOL / 'cores3-bridge-v1.json').read_text())
    assert manifest['uuids'] == {'service': cores3.SERVICE, 'rx': cores3.RX, 'tx': cores3.TX}
    assert manifest['headerSize'] == cores3.HEADER.size == 26
    assert cores3.capabilities(json.dumps(GOLDEN['capabilities'])) == GOLDEN['capabilities']
    for change in ({'protocol': 2}, {'peripherals': []}, {'clientNonce': 'bad'}):
        with pytest.raises(CardError):
            cores3.capabilities(json.dumps(GOLDEN['capabilities'] | change))


class Wire:
    def __init__(self):
        self.queue, self.sent, self.closed = asyncio.Queue(), [], False
        self.drop = False
        self.status = 0
        self.modify = lambda frame: frame
        self.handler = lambda frame: frame.payload
        self.caps = copy.deepcopy(GOLDEN['capabilities'])

    async def send(self, message):
        request, = cores3.Decoder().feed(message)
        self.sent.append(request)
        if self.drop:
            return
        if request.operation == 1 and request.channel == 0:
            self.caps['clientNonce'] = request.payload.hex()
            payload = json.dumps(self.caps).encode()
        else:
            payload = self.handler(request)
        reply = cores3.Frame(1, 12345, request.request_id, request.channel, request.operation,
                             request.generation, self.status, payload)
        await self.queue.put(self.modify(reply).encode())

    async def receive(self):
        value = await self.queue.get()
        if isinstance(value, BaseException):
            raise value
        return value

    async def close(self):
        self.closed = True

    async def event(self, channel=0, payload=None, generation=0, operation=128):
        payload = json.dumps(self.caps).encode() if payload is None else payload
        await self.queue.put(cores3.Frame(2, 12345, 0, channel, operation, generation, payload=payload).encode())
        await asyncio.sleep(.001)


@pytest.mark.parametrize('mode,pending,allowed', [
    ('home', False, False), ('standalone', False, False), ('connected', False, True),
    ('connected', True, False),
])
async def test_modes_gate_all_peripherals_but_preserve_control(mode, pending, allowed):
    wire = Wire()
    wire.caps.update(mode=mode, owner={'home': 'none', 'standalone': 'local', 'connected': 'desktop'}[mode],
                     modePending=pending)
    bridge = cores3.Session(wire)
    try:
        value = await bridge.hello()
        assert cores3.remote_allowed(value) is allowed
        assert cores3.mode_description(value)
        for channel in (1, 2):
            if allowed:
                async with bridge.lease(channel):
                    pass
            else:
                with pytest.raises(CardError, match='Connected mode'):
                    async with bridge.lease(channel):
                        pytest.fail('Mode allowed a reader lease')
                with pytest.raises(CardError, match='Connected mode'):
                    await bridge.request(channel, 16, generation=bridge.slots[channel]['generation'])
        assert (await bridge.request(0, 3, b'ping')).payload == b'ping'
        assert all(frame.channel == 0 for frame in wire.sent)
    finally:
        await bridge.close()


@pytest.mark.parametrize('metadata', [
    {'mode': 'invalid'}, {'mode': 'home', 'owner': 'desktop'},
    {'mode': 'standalone', 'owner': 'desktop'}, {'modePending': True}, {'owner': 'local'},
    {'mode': 'connected', 'modePending': 'false'}, {'mode': ['connected']},
])
def test_invalid_mode_metadata_is_rejected(metadata):
    with pytest.raises(CardError):
        cores3.capabilities(json.dumps(GOLDEN['capabilities'] | metadata))


async def test_mode_pending_preserves_inflight_response_and_revokes_lease(session):
    bridge, wire = session
    async with bridge.lease(1) as endpoint:
        wire.drop = True
        pending = asyncio.create_task(endpoint.request(18, b'write-once'))
        await asyncio.sleep(.001)
        request = wire.sent[-1]
        wire.caps.update(mode='connected', owner='desktop', modePending=True)
        await wire.event()
        assert not pending.done() and not bridge.closed
        with pytest.raises(CardError, match='Connected mode'):
            async with bridge.lease(2):
                pytest.fail('Pending transition granted a lease')
        await wire.queue.put(cores3.Frame(1, 12345, request.request_id, 1, 18, request.generation,
                                         payload=b'verified').encode())
        assert await pending == b'verified'
        assert not bridge.closed and not bridge.leases
        wire.drop = False
        assert (await bridge.request(0, 3, b'control')).payload == b'control'
        wire.caps['modePending'] = False
        await wire.event()
        with pytest.raises(CardError, match='lease expired'):
            await endpoint.request(18, b'write-once')
    assert len([f for f in wire.sent if f.channel]) == 1
    async with bridge.lease(1):
        pass


async def test_mode_change_revokes_idle_lease_without_disconnect(session):
    bridge, wire = session
    async with bridge.lease(1) as endpoint:
        wire.caps.update(mode='standalone', owner='local', modePending=False)
        await wire.event()
        with pytest.raises(CardError, match='lease expired'):
            await endpoint.request(16)
        assert not bridge.closed
        assert (await bridge.request(0, 3, b'ok')).payload == b'ok'


@pytest.fixture
async def session():
    wire = Wire()
    session = cores3.Session(wire)
    await session.hello()
    yield session, wire
    await session.close()


async def test_nonce_bound_hello_and_monotonic_ids(session):
    session, wire = session
    assert session.nonce == 12345
    assert wire.sent[0].session == 0 and len(wire.sent[0].payload) == 8
    for i in range(2, 5):
        reply = await session.request(0, 3, bytes([i]))
        assert reply.payload == bytes([i]) and wire.sent[-1].request_id == i
    session.next_id = 0x100000000
    with pytest.raises(CardError, match='wraparound'):
        await session.request(0, 3)
    assert len(wire.sent) == 4


async def test_hello_rejects_wrong_client_nonce():
    wire = Wire()
    wire.modify = lambda frame: cores3.Frame(1, 123, frame.request_id, 0, 1, payload=json.dumps(
        GOLDEN['capabilities'] | {'clientNonce': '0000000000000000'}).encode())
    session = cores3.Session(wire)
    try:
        with pytest.raises(CardError, match='HELLO mismatch'):
            await session.hello()
    finally:
        await session.close()


@pytest.mark.parametrize('status,error', [(10, NoCard), (11, AuthenticationError), (16, CardError), (17, CardError)])
async def test_statuses_do_not_disguise_transport_failure_as_removal(session, status, error):
    session, wire = session
    wire.status = status
    async with session.lease(1) as endpoint:
        with pytest.raises(error):
            await endpoint.request(16)
    assert not session.closed


async def test_leases_generation_changes_and_no_implicit_reselection(session):
    session, wire = session
    async with session.lease(1) as endpoint:
        with pytest.raises(CardError, match='already in use'):
            async with session.lease(1):
                pass
        wire.caps['peripherals'][0]['generation'] += 1
        await wire.event()
        with pytest.raises(CardError, match='lease expired'):
            await endpoint.request(16)
    assert len(wire.sent) == 1
    async with session.lease(1) as replacement:
        assert replacement.generation == 4
        await replacement.request(16)


async def test_stale_generation_rejected_before_transmission(session):
    session, wire = session
    with pytest.raises(CardError, match='stale'):
        await session.request(1, 16, generation=2)
    assert len(wire.sent) == 1


@pytest.mark.parametrize('failure', ['timeout', 'cancel', 'disconnect', 'wrong-id', 'wrong-session', 'crc'])
async def test_fail_closed_never_replays_or_accepts_late_results(session, failure):
    session, wire = session
    if failure in ('timeout', 'cancel', 'disconnect'):
        wire.drop = True
    elif failure == 'wrong-id':
        wire.modify = lambda f: cores3.Frame(1, f.session, f.request_id+1, f.channel, f.operation, f.generation)
    elif failure == 'wrong-session':
        wire.modify = lambda f: cores3.Frame(1, 99, f.request_id, f.channel, f.operation, f.generation)
    else:
        wire.drop = True
    async with session.lease(1) as endpoint:
        task = asyncio.create_task(endpoint.request(18, b'write-once', timeout=.02))
        await asyncio.sleep(.001)
        if failure == 'cancel':
            task.cancel()
        elif failure == 'disconnect':
            await wire.queue.put(CardError('Disconnected'))
        elif failure == 'crc':
            await wire.queue.put(b'bad packet')
        with pytest.raises((CardError, TimeoutError, asyncio.CancelledError)):
            await task
        assert session.closed and wire.closed and not session.leases and not session.decoder.data
        with pytest.raises(CardError):
            await endpoint.request(18, b'write-once')
    assert len(wire.sent) == 2


async def test_one_pending_operation_and_cancel_fallback(session):
    session, wire = session
    wire.drop = True
    first = asyncio.create_task(session.request(0, 3))
    await asyncio.sleep(.001)
    with pytest.raises(CardError, match='busy'):
        await session.request(0, 3)
    first.cancel()
    with pytest.raises(asyncio.CancelledError):
        await first
    assert wire.closed


async def test_generation_change_during_write_closes_session(session):
    session, wire = session
    wire.drop = True
    async with session.lease(1) as endpoint:
        task = asyncio.create_task(endpoint.request(18, b'one write'))
        await asyncio.sleep(.001)
        wire.caps['peripherals'][0]['generation'] += 1
        await wire.event()
        with pytest.raises(CardError, match='unverified'):
            await task
    assert session.closed and len(wire.sent) == 2


async def test_raw_m100_identify_inventory_notifications_and_no_tag(session):
    session, wire = session
    async with session.lease(2) as endpoint:
        connection = cores3.RawConnection(endpoint, m100.Decoder(), m100.encode)
        client = m100.M100(connection)
        wire.handler = lambda _: m100.encode(3, b'\0JRD-4035', kind=1)
        assert 'JRD' in await client.connect()
        wire.handler = lambda _: m100.encode(0xff, b'\x15', kind=1)
        assert await client.command(0x22) == b''
        tags = []
        client.on_tag = tags.append
        report = bytes.fromhex('C9340030751FEB705C5904E3D50D703A76')
        wire.handler = lambda _: m100.encode(0x22, report, kind=2)
        assert await client.command(0x22) == b'' and len(tags) == 1
        await wire.event(2, m100.encode(0x22, report, kind=2), 7, 32)
        assert len(tags) == 2
        wire.handler = lambda _: m100.encode(0xff, b'\x16', kind=1)
        with pytest.raises(CardError, match='password'):
            await client.command(0x39, m100.read_payload(3, 0, 1, bytes(4)))
        await connection.close()


async def test_raw_chameleon_preserves_real_capabilities_mode_and_allowlist(session):
    session, wire = session
    wire.caps['peripherals'][2].update(present=True, capabilities=['chameleon'])
    await wire.event()
    commands = {1000, 1001, 1002, 1033, 1035, 2000, 2008, 2009, 3000}
    def handler(request):
        command = int.from_bytes(request.payload[2:4], 'big')
        payload = {1000: b'\2\0', 1033: b'\0', 1035: b''.join(c.to_bytes(2, 'big') for c in commands),
                   1002: b'\0', 1001: b''}.get(command, b'')
        return chameleon.encode(command, payload, status=0x68)
    wire.handler = handler
    async with session.lease(3) as endpoint:
        connection = cores3.RawConnection(endpoint, chameleon.Decoder(), chameleon.encode)
        client = chameleon.Chameleon(connection)
        await client.connect()
        assert client.capabilities == commands
        before = len(wire.sent)
        with pytest.raises(CardError, match='not allowed'):
            await connection.exchange(2010, b'raw ISO')
        assert len(wire.sent) == before
        await client.close()
        assert wire.sent[-1].payload == chameleon.encode(1001, b'\0')


async def test_ble_protected_service_pairing_subscription_and_small_writes(monkeypatch):
    calls = []
    class FakeClient:
        is_connected = True
        mtu_size = 15
        def __init__(self, device, **kwargs):
            calls.append(('init', kwargs))
            self.services = SimpleNamespace(get_characteristic=lambda uuid: SimpleNamespace(
                properties=['write'] if uuid == cores3.RX else ['indicate']))
        async def connect(self):
            calls.append(('connect',))
        async def start_notify(self, tx, callback):
            calls.append(('subscribe', tx))
        async def write_gatt_char(self, rx, data, response):
            calls.append(('write', rx, data, response))
        async def disconnect(self):
            self.is_connected = False
    monkeypatch.setitem(sys.modules, 'bleak', SimpleNamespace(BleakClient=FakeClient))
    monkeypatch.setattr(cores3.sys, 'platform', 'linux')
    ble = await cores3.BLE.connect('fake-device', lambda _: None)
    await ble.send(bytes(35))
    assert calls[0][1]['pair'] is True
    writes = [c for c in calls if c[0] == 'write']
    assert [len(c[2]) for c in writes] == [12, 12, 11]
    assert all(c[1] == cores3.RX and c[3] for c in writes)
    await ble.close()


async def test_ble_never_falls_back_to_unprotected_uart(monkeypatch):
    class Fake:
        is_connected = True
        services = SimpleNamespace(get_characteristic=lambda _: None)
        def __init__(self, *_args, **_kwargs):
            pass
        async def connect(self):
            pass
        async def disconnect(self):
            self.is_connected = False
    monkeypatch.setitem(sys.modules, 'bleak', SimpleNamespace(BleakClient=Fake))
    with pytest.raises(CardError, match='Protected'):
        await cores3.BLE.connect('fake', lambda _: None)


async def test_service_reuses_shared_session_and_releases_hf_credentials(session):
    bridge, wire = session
    service = Service(lambda _: None)
    service.bridge = bridge
    identity = {'uid': '01020304', 'atqa': '0400', 'sak': 8, 'family': 'classic1k',
                'blockCount': 64, 'pageCount': 0, 'version': ''}
    wire.handler = lambda request: b'' if request.operation == 21 else json.dumps(identity).encode()
    async with service.hf_session(cores3.HF_READER) as client:
        backend = client.backend
        assert isinstance(client, cores3.HFClient)
        assert backend.endpoint.session is bridge and 1 in bridge.leases
    assert backend.authentication == b'\0' and backend.card is None and not bridge.leases
    devices = await service.discover('CoreS3', 'uhf')
    assert devices[0][0] == 2
    assert wire.sent[-1].operation == 21


@pytest.mark.parametrize('channel,command,data', [
    (2, 0x07, b'\1'), (2, 0x82, bytes(4)), (2, 0x65, bytes(4)),
    (2, 0xb6, (2700).to_bytes(2, 'big')), (2, 0x49, bytes(11)),
    (2, 0x49, bytes(4)+b'\2\0\0\0\1xx'),
    (2, 0x49, bytes(4)+b'\1\0\0\0\1xx'), (2, 0x39, bytes(8)),
    (3, 2010, b''), (3, 3001, bytes(11)), (3, 3016, bytes([7])+bytes(10)),
    (3, 3016, b'\0\0\0\0\x10'+bytes(6)), (3, 2009, b'\x60\0'+bytes(22)),
    (3, 2009, b'\x60\xff'+bytes(22)), (3, 1001, b'\2'),
])
def test_raw_policies_reject_unsafe_or_unimplemented_operations(channel, command, data):
    with pytest.raises(CardError):
        cores3.guard_raw(channel, command, data)


class TypedWire(Wire):
    def __init__(self, card):
        super().__init__()
        self.card = card
        self.handler = self.operation

    def operation(self, request):
        card, payload = self.card, request.payload
        info = card.info
        if request.operation == 16:
            return json.dumps({'uid': info.uid.hex(), 'atqa': info.atqa.hex(), 'sak': info.sak,
                               'family': info.family, 'blockCount': info.unit_count if info.classic else 0,
                               'pageCount': info.unit_count if info.ntag else 0, 'version': info.version.hex()}).encode()
        if request.operation == 21:
            return b''
        uid = bytes([len(info.uid)])+info.uid
        assert payload.startswith(uid)
        payload = payload[len(uid):]
        number = int.from_bytes(payload[:2], 'little')
        if request.operation == 17:
            assert len(payload) == 9 and payload[2] in (0x60, 0x61)
            return card.memory[number]
        if request.operation == 18:
            assert len(payload) == 41 and hf.data_block(number, info.unit_count)
            assert card.memory[number] == payload[9:25]
            card.memory[number] = payload[25:]
            card.writes.append((number, payload[25:]))
            return payload[25:]
        count, auth = (payload[2], payload[3:]) if request.operation == 19 else (1, payload[2:-8])
        assert auth in (b'\0', b'\1\x11\x22\x33\x44', b'\2\x11\x22\x33\x44\x12\x34')
        if request.operation == 19:
            assert 1 <= count <= 16 and number+count <= info.unit_count
            return b''.join(card.pages[p] for p in range(number, number+count))
        assert request.operation == 20 and number in hf.eligible_units(info.family)
        assert card.pages[number] == payload[-8:-4]
        card.pages[number] = payload[-4:]
        card.writes.append((number, payload[-4:]))
        return payload[-4:]


async def test_typed_classic_payloads_compare_before_write_and_protected_blocks():
    from test_hf_families import Classic4K
    card, service = Classic4K(), Service(lambda _: None)
    wire = TypedWire(card)
    service.bridge = cores3.Session(wire)
    try:
        await service.bridge.hello()
        snapshot = await service.hf_read(cores3.HF_READER, hf.FACTORY)
        assert len(snapshot.blocks) == 256
        async with service.hf_session(cores3.HF_READER) as client:
            await client.write_block(128, b'\xaa'*16, snapshot)
        assert card.writes == [(128, b'\xaa'*16)]
        request = next(r for r in wire.sent if r.operation == 18)
        assert request.generation == 3 and request.payload[5:8] == b'\x80\0\x60'
        assert request.payload[-32:] == bytes(16) + b'\xaa'*16
        async with service.hf_session(cores3.HF_READER) as client:
            for block in (0, 143, 255):
                with pytest.raises(CardError):
                    await client.write_block(block, bytes(16), snapshot)
    finally:
        await service.close()


async def test_hello_and_immediate_capabilities_event_in_one_chunk():
    class Burst(Wire):
        async def send(self, message):
            await super().send(message)
            hello = await self.queue.get()
            self.caps['peripherals'][0]['generation'] = 8
            event = cores3.Frame(2, 12345, 0, 0, 128, payload=json.dumps(self.caps).encode()).encode()
            await self.queue.put(hello + event)
    wire, changes = Burst(), []
    session = cores3.Session(wire, changes.append)
    try:
        await session.hello()
        assert not session.closed and session.nonce == 12345 and session.slots[1]['generation'] == 8
        assert len(changes) == 2
    finally:
        await session.close()


async def test_transmission_itself_is_bounded_and_not_replayed(session):
    bridge, wire = session
    sent = []
    async def hung_send(data):
        sent.append(data)
        await asyncio.Event().wait()
    wire.send = hung_send
    with pytest.raises(TimeoutError):
        await bridge.request(0, 3, timeout=.01)
    assert bridge.closed and len(sent) == 1


@pytest.mark.parametrize('status', [1, 2, 3, 4, 5, 6, 8, 9, 14, 15])
async def test_transport_statuses_fail_closed(session, status):
    bridge, wire = session
    wire.status = status
    with pytest.raises(CardError):
        await bridge.request(0, 3)
    assert bridge.closed


@pytest.mark.parametrize('metadata', [
    {'family': 'classic1k', 'sak': 0x18}, {'family': 'classic1k', 'atqa': '0200'},
    {'family': 'classic1k', 'atqa': '0401'}, {'family': 'classic4k', 'blockCount': 64},
    {'family': 'classic4k', 'blockCount': 256, 'sak': 0x18, 'atqa': '0400'},
    {'family': 'ntag213', 'blockCount': 0, 'pageCount': 45, 'uid': '04010203040506',
     'atqa': '4400', 'sak': 0, 'version': '0004040201001103'},
    {'family': 'ntag213', 'blockCount': 0, 'pageCount': 45, 'uid': '04010203040506',
     'atqa': '4400', 'sak': 0, 'version': '0003030201000f03'},
])
async def test_inconsistent_card_metadata_closes_bridge(session, metadata):
    bridge, wire = session
    identity = {'uid': '01020304', 'atqa': '0400', 'sak': 8, 'family': 'classic1k',
                'blockCount': 64, 'pageCount': 0, 'version': ''} | metadata
    wire.handler = lambda _: json.dumps(identity).encode()
    async with bridge.lease(1) as endpoint:
        backend = cores3.HF(endpoint)
        with pytest.raises(CardError, match='identity'):
            await backend.identify()
        assert bridge.closed and backend.card is None


async def test_malformed_inner_raw_frame_closes_session(session):
    bridge, wire = session
    async with bridge.lease(2) as endpoint:
        raw = cores3.RawConnection(endpoint, m100.Decoder(), m100.encode)
        wire.handler = lambda _: b'\xbb\1\3\0\1\0\0\x7e'
        with pytest.raises(CardError):
            await raw.exchange(3, b'\0')
        assert raw.closed and bridge.closed and raw.decoder.data == b''


async def test_chameleon_hf_lf_lease_cannot_interleave():
    client = chameleon.Chameleon(SimpleNamespace(closed=False))
    async with client.lease():
        async def competing():
            async with client.lease():
                pytest.fail('Competing HF/LF operation acquired the lease')
        with pytest.raises(CardError, match='already in use'):
            await asyncio.create_task(competing())
        async with client.lease():
            assert client.depth == 2
    assert client.owner is None and client.depth == 0


@pytest.mark.parametrize('delay,limit,success', [(.015, .1, True), (.1, .01, False)])
async def test_pairing_hello_has_configurable_bounded_setup_without_retries(delay, limit, success):
    wire = Wire()
    original = wire.send
    calls = []
    async def delayed(data):
        calls.append(data)
        await asyncio.sleep(delay)
        await original(data)
    wire.send = delayed
    bridge = cores3.Session(wire)
    try:
        if success:
            await bridge.hello(timeout=limit)
            assert bridge.nonce and not bridge.closed
        else:
            with pytest.raises(TimeoutError):
                await bridge.hello(timeout=limit)
            assert bridge.closed
        assert len(calls) == 1
    finally:
        await bridge.close()


async def test_shared_connection_hello_uses_90_second_setup_bound(monkeypatch):
    wire, limits = Wire(), []
    async def connect(*_args):
        return wire
    original = cores3.Session.hello
    async def hello(self, timeout=90):
        limits.append(timeout)
        return await original(self, timeout)
    monkeypatch.setattr(cores3.BLE, 'connect', connect)
    monkeypatch.setattr(cores3.Session, 'hello', hello)
    bridge = await cores3.Session.connect('fake-only', lambda _: None)
    assert limits == [90]
    await bridge.close()


@pytest.mark.parametrize('auth', [b'\0', b'\1\x11\x22\x33\x44', b'\2\x11\x22\x33\x44\x12\x34'])
async def test_typed_ntag_auth_payloads_and_credentials_cleared(auth):
    from test_hf_families import NTAG
    card, service = NTAG(), Service(lambda _: None)
    wire = TypedWire(card)
    service.bridge = cores3.Session(wire)
    try:
        await service.bridge.hello()
        snapshot = await service.hf_read(cores3.HF_READER, hf.FACTORY, auth)
        async with service.hf_session(cores3.HF_READER, auth) as client:
            backend = client.backend
            await client.write_block(6, b'ABCD', snapshot)
        assert card.writes == [(6, b'ABCD')]
        assert backend.authentication == b'\0' and not backend.previous
        assert any(r.operation == 20 and r.payload[-4:] == b'ABCD' for r in wire.sent)
        wire.caps['peripherals'][0]['capabilities'].remove('ntag_auth')
        await wire.event()
        if auth != b'\0':
            with pytest.raises(CardError, match='password'):
                await service.hf_read(cores3.HF_READER, hf.FACTORY, auth)
    finally:
        await service.close()


async def test_service_routes_m100_on_existing_bridge_without_new_ble_link(session):
    bridge, wire = session
    service = Service(lambda _: None)
    service.bridge = bridge
    wire.handler = lambda _: m100.encode(3, b'\0JRD-4035', kind=1)
    assert 'JRD' in await service.connect('uhf', 'CoreS3', 2, 115200)
    assert service.uhf.connection.endpoint.session is bridge
    assert wire.sent[-1].channel == 2
    await service.disconnect('uhf')
    assert not bridge.closed and not bridge.leases


async def test_bridge_batch_observes_no_card_instead_of_transport_failure():
    from test_hf_families import NTAG
    from quadband import ntag
    from quadband.service import StopFlag
    card = NTAG()
    profile = hf.profile_from_snapshot(await ntag.Client(card).read())
    card.pages[6] = b'xxxx'
    wire, service, stop, results = TypedWire(card), Service(lambda _: None), StopFlag(), []
    states = iter((True, False, True))
    present = True
    def handler(request):
        nonlocal present
        if request.operation == 21:
            wire.status = 0
            return b''
        if request.operation == 16:
            present = next(states, True)
        wire.status = 0 if present else 10
        return wire.operation(request) if present else b'Place one card'
    wire.handler = handler
    service.bridge = cores3.Session(wire)
    try:
        await service.bridge.hello()
        def result(success, detail):
            results.append((success, detail))
            stop.set()
        await asyncio.wait_for(hf.copy_forever(lambda: service.hf_session(cores3.HF_READER),
            profile, hf.FACTORY, stop, lambda _: None, result, .001), 5)
        assert results[0][0] and '36 data units' in results[0][1]
        assert card.writes == [(6, bytes.fromhex(profile['blocks'][2]['hex']))]
    finally:
        await service.close()
