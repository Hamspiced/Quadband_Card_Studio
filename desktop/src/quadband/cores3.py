"""CoreS3 bridge v1. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from contextlib import asynccontextmanager
from dataclasses import dataclass
import json
import secrets
import struct
import sys
import zlib
from .common import CardError, NoCard, AuthenticationError, require
from .hf import FACTORY

SERVICE = 'b8878a55-59c6-567c-a112-f35a26093466'
RX = '82a2c3eb-e3c7-50aa-8701-ed1388a3b3a3'
TX = '37c9a177-5355-5f31-b1e3-808ccf30f92e'
HF_READER = 'CoreS3 · Grove HF'
HEADER = struct.Struct('<2sBBQIBBHIH')


@dataclass(frozen=True)
class Frame:
    kind: int
    session: int
    request_id: int
    channel: int
    operation: int
    generation: int = 0
    status: int = 0
    payload: bytes = b''

    def encode(self):
        require(self.kind in (0, 1, 2) and len(self.payload) <= 2048, 'Invalid CoreS3 frame.')
        data = HEADER.pack(b'QB', 1, self.kind, self.session, self.request_id,
                           self.channel, self.operation, self.status, self.generation, len(self.payload))
        data += self.payload
        return data + struct.pack('<I', zlib.crc32(data))


class Decoder:
    def __init__(self):
        self.data = b''

    def feed(self, chunk):
        self.data += chunk
        try:
            require(len(self.data) <= 8312, 'CoreS3 receive buffer exceeded.')
            frames = []
            while self.data:
                require(self.data[:1] == b'Q', 'CoreS3 framing lost.')
                if len(self.data) < HEADER.size:
                    break
                magic, version, kind, session, rid, channel, op, status, gen, size = HEADER.unpack_from(self.data)
                require(magic == b'QB' and version == 1 and kind in (0, 1, 2) and size <= 2048,
                        'Invalid CoreS3 frame header.')
                length = HEADER.size + size + 4
                if len(self.data) < length:
                    break
                require(zlib.crc32(self.data[:length-4]) == struct.unpack_from('<I', self.data, length-4)[0],
                        'CoreS3 CRC failed.')
                frames.append(Frame(kind, session, rid, channel, op, gen, status,
                                    self.data[HEADER.size:length-4]))
                self.data = self.data[length:]
            return frames
        except BaseException:
            self.data = b''
            raise


def capabilities(payload):
    try:
        value = json.loads(payload)
        require(type(value['protocol']) is int and value['protocol'] == 1 and all(isinstance(value[k], str) for k in
                ('deviceId', 'name', 'firmware', 'clientNonce')), 'Invalid CoreS3 capabilities.')
        require(len(bytes.fromhex(value['clientNonce'])) == 8, 'Invalid client nonce.')
        if 'mode' in value:
            owners = {'home': 'none', 'connected': 'desktop', 'standalone': 'local'}
            require(isinstance(value['mode'], str) and value['mode'] in owners
                    and value.get('owner', owners[value['mode']]) == owners[value['mode']],
                    'Invalid CoreS3 mode ownership.')
            require('modePending' not in value or type(value['modePending']) is bool,
                    'Invalid CoreS3 mode transition.')
        else:
            require('owner' not in value and 'modePending' not in value, 'Incomplete CoreS3 mode metadata.')
        slots = value['peripherals']
        require(len(slots) == 3 and {p['channel'] for p in slots} == {1, 2, 3},
                'CoreS3 must report all peripheral slots.')
        for p in slots:
            require(type(p['channel']) is int and type(p['generation']) is int and 0 < p['generation'] <= 0xffffffff
                    and type(p['present']) is bool and isinstance(p['kind'], str)
                    and isinstance(p['capabilities'], list)
                    and all(isinstance(c, str) for c in p['capabilities']), 'Invalid peripheral capabilities.')
        return value
    except (KeyError, ValueError, TypeError) as exc:
        raise CardError('Invalid CoreS3 capabilities.') from exc


def remote_allowed(value):
    return value is not None and value.get('mode', 'connected') == 'connected' and not value.get('modePending', False)


def mode_description(value):
    if value is None:
        return 'CoreS3 disconnected'
    if value.get('modePending'):
        return 'Switching CoreS3 modes — readers paused'
    return {'home': 'Choose Connected mode on the CoreS3',
            'standalone': 'Standalone mode — local ownership',
            'connected': 'Connected mode — desktop reader control'}[value.get('mode', 'connected')]


async def discover():
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=6, return_adv=True, service_uuids=[SERVICE])
    return [(device, f'{adv.local_name or device.name or "CoreS3"} · {device.address}')
            for device, adv in found.values() if SERVICE in [s.lower() for s in adv.service_uuids]]


class BLE:
    def __init__(self):
        self.client = None
        self.queue = asyncio.Queue(maxsize=64)

    @classmethod
    async def connect(cls, device, progress):
        from bleak import BleakClient
        self = cls()
        def incoming(_, data):
            if self.queue.full():
                self.queue.get_nowait()
                self.queue.put_nowait(CardError('CoreS3 receive queue overflow.'))
            else:
                self.queue.put_nowait(bytes(data))
        def disconnected(_):
            if self.queue.full():
                self.queue.get_nowait()
            self.queue.put_nowait(CardError('CoreS3 disconnected; sent writes are unverified.'))
        self.client = BleakClient(device, pair=sys.platform != 'darwin', timeout=90,
                                  disconnected_callback=disconnected)
        try:
            progress('Authorize pairing on CoreS3 and confirm the displayed passkey in the system prompt.')
            await self.client.connect()
            rx = self.client.services.get_characteristic(RX)
            tx = self.client.services.get_characteristic(TX)
            require(rx is not None and 'write' in rx.properties and tx is not None
                    and 'indicate' in tx.properties, 'Protected CoreS3 GATT service unavailable.')
            # Protected CCCD subscription triggers OS authentication on macOS.
            await self.client.start_notify(tx, incoming)
            return self
        except BaseException:
            await self.close()
            raise

    async def send(self, message):
        require(self.client and self.client.is_connected, 'CoreS3 is disconnected.')
        size = min(20, max(1, self.client.mtu_size - 3))
        for offset in range(0, len(message), size):
            await self.client.write_gatt_char(RX, message[offset:offset+size], response=True)

    async def receive(self):
        item = await self.queue.get()
        if isinstance(item, Exception):
            raise item
        return item

    async def close(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()
        while not self.queue.empty():
            self.queue.get_nowait()


class Session:
    def __init__(self, channel, changed=lambda _: None):
        self.channel, self.changed = channel, changed
        self.decoder = Decoder()
        self.nonce, self.next_id = 0, 1
        self.client_nonce = None
        self.description = None
        self.pending = None
        self.closed = False
        self.slots = {}
        self.leases = {}
        self.events = {}
        self.description = None
        self.reader = asyncio.create_task(self._read())

    @classmethod
    async def connect(cls, device, progress, changed=lambda _: None):
        self = None
        try:
            async with asyncio.timeout(90):
                self = cls(await BLE.connect(device, progress), changed)
                await self.hello(timeout=90)
            return self
        except BaseException:
            if self:
                await self.close()
            raise

    async def hello(self, timeout=90):
        require(self.nonce == 0, 'A CoreS3 link may establish only one desktop session.')
        nonce = secrets.token_bytes(8)
        self.client_nonce = nonce.hex()
        reply = await self.request(0, 1, nonce, timeout=timeout)
        return capabilities(reply.payload)

    def _capabilities(self, value):
        require(value['clientNonce'] == self.client_nonce, 'CoreS3 capabilities nonce mismatch.')
        slots = {p['channel']: p for p in value['peripherals']}
        for channel, old in self.slots.items():
            if slots[channel] != old:
                self.leases.pop(channel, None)
                self.events.pop(channel, None)
                if self.pending and self.pending[0].channel == channel:
                    future = self.pending[1]
                    if not future.done():
                        future.set_exception(CardError('CoreS3 peripheral changed; write outcome unverified.'))
        self.slots = slots
        self.description = value
        if not remote_allowed(value) and (not self.pending or self.pending[0].channel == 0):
            self.leases.clear()
            self.events.clear()
        self.changed(value)

    async def _read(self):
        try:
            while True:
                for frame in self.decoder.feed(await self.channel.receive()):
                    if frame.kind == 2:
                        require(self.nonce != 0 and frame.session == self.nonce and frame.request_id == 0
                                and frame.status == 0, 'Invalid CoreS3 event.')
                        if frame.channel == 0 and frame.operation == 128 and frame.generation == 0:
                            self._capabilities(capabilities(frame.payload))
                        elif (frame.channel == 2 and frame.operation == 32
                              and frame.generation == self.slots[2]['generation']):
                            callback = self.events.get(2)
                            if callback:
                                callback(frame.payload)
                        else:
                            raise CardError('Unexpected CoreS3 event.')
                        continue
                    require(frame.kind == 1 and self.pending is not None, 'Unsolicited CoreS3 response.')
                    request, future = self.pending
                    require((frame.request_id, frame.channel, frame.operation, frame.generation) ==
                            (request.request_id, request.channel, request.operation, request.generation)
                            and (request.operation == 1 and request.channel == 0 or frame.session == self.nonce),
                            'CoreS3 response correlation failed.')
                    require(not future.done(), 'Duplicate CoreS3 response.')
                    if request.channel == 0 and request.operation == 1 and not frame.status:
                        value = capabilities(frame.payload)
                        require(frame.session != 0 and value['clientNonce'] == self.client_nonce,
                                'CoreS3 HELLO mismatch.')
                        self.nonce = frame.session
                        self._capabilities(value)
                    elif request.channel == 0 and request.operation == 2 and not frame.status:
                        self._capabilities(capabilities(frame.payload))
                    future.set_result(frame)
        except asyncio.CancelledError:
            return
        except Exception as exc:
            if self.pending and not self.pending[1].done():
                self.pending[1].set_exception(exc)
            await self.close()

    async def request(self, channel, operation, payload=b'', generation=0, timeout=5):
        require(not self.closed and self.pending is None, 'CoreS3 unavailable or busy.')
        require(self.next_id <= 0xffffffff, 'Reconnect CoreS3 before request ID wraparound.')
        require(self.nonce != 0 or (channel, operation) == (0, 1), 'CoreS3 needs HELLO.')
        if channel == 0:
            require(generation == 0 and operation in (1, 2, 3, 4), 'Invalid CoreS3 control operation.')
            require((operation == 1 and len(payload) == 8) or (operation == 2 and not payload)
                    or (operation == 3 and len(payload) <= 64) or (operation == 4 and len(payload) == 4),
                    'Invalid CoreS3 control payload.')
        if channel:
            require(remote_allowed(self.description), 'Select Connected mode on the CoreS3 before using its readers.')
            slot = self.slots.get(channel)
            require(slot and slot['present'] and generation == slot['generation'], 'CoreS3 stale peripheral.')
        frame = Frame(0, self.nonce, self.next_id, channel, operation, generation, payload=payload)
        self.next_id += 1
        future = asyncio.get_running_loop().create_future()
        self.pending = (frame, future)
        try:
            async with asyncio.timeout(timeout):
                await self.channel.send(frame.encode())
                reply = await future
            if reply.status:
                if reply.status in (1, 2, 3, 4, 5, 6, 8, 9, 14, 15):
                    await self.close()
                error = NoCard if reply.status == 10 else AuthenticationError if reply.status == 11 else CardError
                raise error(f'CoreS3 status {reply.status}: {reply.payload.decode("utf-8", "replace")[:200]}')
            return reply
        except (asyncio.CancelledError, TimeoutError):
            await self.close()
            raise
        except Exception:
            # No replay, including partial sends or stale generation responses.
            if not future.done() or future.exception() is not None:
                await self.close()
            raise
        finally:
            if future.done() and not future.cancelled():
                future.exception()
            self.pending = None
            if not remote_allowed(self.description):
                self.leases.clear()
                self.events.clear()

    @asynccontextmanager
    async def lease(self, channel):
        require(remote_allowed(self.description), 'Select Connected mode on the CoreS3 before using its readers.')
        require(not self.closed and channel not in self.leases, 'CoreS3 endpoint is already in use.')
        slot = self.slots.get(channel)
        require(slot and slot['present'], 'CoreS3 peripheral unavailable.')
        token = object()
        self.leases[channel] = token
        try:
            yield Endpoint(self, channel, slot['generation'], token)
        finally:
            if self.leases.get(channel) is token:
                self.leases.pop(channel)

    async def close(self):
        if self.closed:
            return
        self.closed = True
        self.nonce = 0
        self.client_nonce = None
        self.slots.clear()
        self.leases.clear()
        self.events.clear()
        self.decoder.data = b''
        if self.reader is not asyncio.current_task():
            self.reader.cancel()
        if self.pending and not self.pending[1].done():
            self.pending[1].set_exception(CardError('CoreS3 disconnected; write outcome unverified.'))
        await self.channel.close()
        self.changed(None)


class Endpoint:
    def __init__(self, session, channel, generation, token):
        self.session, self.channel, self.generation, self.token = session, channel, generation, token

    async def request(self, operation, payload=b'', timeout=5):
        require(self.session.leases.get(self.channel) is self.token, 'CoreS3 lease expired; reselect reader.')
        return (await self.session.request(self.channel, operation, payload, self.generation, timeout)).payload


def guard_raw(channel, command, data):
    from . import m100, t5577
    from .hf import data_block
    if channel == 2:
        require(command in {3, 8, 11, 12, 18, 34, 40, 57, 73, 182, 183},
                'UHF command is not allowed through CoreS3.')
        if command == 3:
            require(data in (b'\0', b'\1', b'\2'), 'Invalid UHF identification.')
        elif command == 12:
            require(len(data) >= 11 and data == m100.select_payload(data[9:], data[7:9]),
                    'Invalid UHF exact selection.')
        elif command == 18:
            require(data in (b'\0', b'\1', b'\2'), 'Invalid UHF selection mode.')
        elif command in (57, 73):
            require(len(data) >= 9, 'Invalid UHF memory payload.')
            bank, word, count = data[4], int.from_bytes(data[5:7], 'big'), int.from_bytes(data[7:9], 'big')
            if command == 57:
                require(data == m100.read_payload(bank, word, count, data[:4]), 'Invalid UHF read payload.')
            else:
                require(len(data) == 9 + count * 2 and
                        data == m100.write_payload(bank, word, data[9:], data[:4]), 'Invalid UHF write payload.')
        elif command == 182:
            require(len(data) == 2 and 1800 <= int.from_bytes(data, 'big') <= 2600, 'Invalid UHF RF power.')
        else:
            require(not data, 'Unexpected UHF command payload.')
    else:
        require(channel == 3 and command in {1000, 1001, 1002, 1017, 1033, 1035, 2000, 2008, 2009,
                                             3000, 3002, 3004, 3010, 3014, 3016, 3019, 3021},
                'Command is not allowed through CoreS3.')
        if command == 1001:
            require(data in (b'\0', b'\1'), 'Invalid Chameleon reader mode.')
        elif command in (2008, 2009):
            require(len(data) == (8 if command == 2008 else 24) and data[0] in (0x60, 0x61)
                    and (command != 2009 or data_block(data[1], 256)), 'Invalid Classic raw command.')
        elif command == 3016:
            t5577.guard(command, data)
        elif command == 3002:
            require(len(data) == 1 and data[0] <= 42, 'Invalid HID scan hint.')
        elif command == 3010:
            require(data == b'\0', 'Invalid ioProx scan hint.')
        else:
            require(not data, 'Unexpected Chameleon command payload.')


class RawConnection:
    @property
    def closed(self):
        return (self._closed or self.endpoint.session.closed
                or self.endpoint.session.leases.get(self.endpoint.channel) is not self.endpoint.token)

    @closed.setter
    def closed(self, value):
        self._closed = value
    def __init__(self, endpoint, decoder, encode):
        self.endpoint, self.decoder, self.encode = endpoint, decoder, encode
        self.on_frame = lambda _: None
        self.closed = False
        self.pending = None
        if endpoint.channel == 2:
            endpoint.session.events[2] = self._event

    def _event(self, payload):
        frames = self.decoder.feed(payload)
        require(len(frames) == 1 and not self.decoder.data, 'Incomplete CoreS3 device event.')
        for frame in frames:
            require(frame.kind == 2 and frame.command == 0x22 and self.on_frame(frame),
                    'Unexpected CoreS3 UHF event.')

    async def exchange(self, command, data=b'', timeout=3):
        require(not self.closed and self.pending is None, 'CoreS3 endpoint closed or busy.')
        guard_raw(self.endpoint.channel, command, data)
        future = asyncio.get_running_loop().create_future()
        self.pending = (command, future)
        try:
            payload = await self.endpoint.request(32, self.encode(command, data), timeout)
            try:
                frames = self.decoder.feed(payload)
                require(len(frames) == 1 and not self.decoder.data, 'CoreS3 raw response mismatch.')
                frame = frames[0]
                if not self.on_frame(frame):
                    require(frame.command == command, 'CoreS3 device command mismatch.')
                    if not future.done():
                        future.set_result(frame)
                require(future.done(), 'CoreS3 device response did not complete the transaction.')
            except Exception:
                self.decoder.data = b''
                await self.endpoint.session.close()
                await self.close()
                raise
            return future.result()
        finally:
            if not future.done():
                future.cancel()
            elif not future.cancelled():
                future.exception()
            self.pending = None

    async def close(self):
        self.closed = True
        self.endpoint.session.events.pop(self.endpoint.channel, None)


class HFClient:
    def __init__(self, backend):
        self.backend = backend

    async def client(self):
        from . import hf, ntag
        card = await self.backend.identify()
        return ntag.Client(self.backend) if card.ntag else hf.ClassicClient(self.backend)

    async def read(self, supplied=FACTORY):
        return await (await self.client()).read(supplied)

    async def write_block(self, block, data, snapshot):
        return await (await self.client()).write_block(block, data, snapshot)

    async def write_profile(self, profile, supplied=FACTORY):
        return await (await self.client()).write_profile(profile, supplied)

    async def write_records(self, records, expected, supplied=FACTORY):
        return await (await self.client()).write_records(records, expected, supplied)

    async def canonical_format(self, snapshot, overwrite=False):
        require(hasattr(self.backend, 'canonical_format'), 'Reader does not support canonical formatting.')
        return await self.backend.canonical_format(snapshot, overwrite)


class HF:
    def __init__(self, endpoint, authentication=b'\0'):
        self.endpoint = endpoint
        require(len(authentication) in (1, 5, 7) and authentication[0] == {1: 0, 5: 1, 7: 2}[len(authentication)],
                'Invalid supplied NTAG credential.')
        self.authentication = authentication
        self.card = None
        self.previous = {}

    def uid(self):
        require(self.card is not None, 'Identify the card first.')
        return bytes([len(self.card.uid)]) + self.card.uid

    async def response(self, operation, payload, size, expected=None):
        result = await self.endpoint.request(operation, payload)
        if len(result) != size or expected is not None and result != expected:
            await self.endpoint.session.close()
            await self.close()
            raise CardError('Malformed or unverified CoreS3 HF response; read again after reconnecting.')
        return result

    async def identify(self):
        from .hf import CardInfo
        payload = await self.endpoint.request(16)
        try:
            value = json.loads(payload)
            card = CardInfo(bytes.fromhex(value['uid']), atqa=bytes.fromhex(value['atqa']),
                            sak=value['sak'], family_hint=value['family'], version=bytes.fromhex(value['version']))
            require(len(card.uid) in (4, 7, 10) and len(card.atqa) == 2 and type(card.sak) is int
                    and 0 <= card.sak <= 255 and card.family in
                    ('classic1k', 'classic4k', 'ntag213', 'ntag215', 'ntag216', 'unknown'),
                    'Invalid HF identity.')
            require(value['blockCount'] == (card.unit_count if card.classic else 0)
                    and value['pageCount'] == (card.unit_count if card.ntag else 0)
                    and (not card.ntag or len(card.version) == 8), 'HF identity geometry mismatch.')
            if card.classic:
                require(len(card.uid) in (4, 7) and card.atqa[1] == 0
                        and card.sak == (8 if card.family == 'classic1k' else 0x18)
                        and card.atqa[0] & 31 == (4 if card.family == 'classic1k' else 2),
                        'Inconsistent Classic identity.')
            elif card.ntag:
                storage = {'ntag213': 15, 'ntag215': 17, 'ntag216': 19}[card.family]
                require(len(card.uid) == 7 and card.sak == 0 and card.atqa == b'\x44\0'
                        and card.version == bytes.fromhex('000404020100') + bytes([storage, 3]),
                        'Unrecognized NTAG product/version; no writable layout can be assumed.')
            if self.card != card:
                self.previous.clear()
            self.card = card
            return card
        except (KeyError, TypeError, ValueError, CardError) as exc:
            await self.endpoint.session.close()
            await self.close()
            raise CardError('Invalid CoreS3 HF identity.') from exc

    def supports(self, capability):
        slot = self.endpoint.session.slots.get(1, {})
        return slot.get('kind') in ('rfid', 'rfid2') and capability in slot.get('capabilities', [])

    async def present(self):
        try:
            await self.identify()
            return True
        except NoCard:
            return False

    async def read_block(self, block, credential):
        require(self.card and self.card.classic and 0 <= block < self.card.unit_count
                and self.supports(self.card.family), 'Classic read unavailable.')
        key, kind = credential
        require(len(key) == 6 and kind in (0x60, 0x61), 'Invalid Classic key.')
        result = await self.response(17, self.uid() + struct.pack('<HB', block, kind) + key, 16)
        self.previous[block] = result
        return result

    async def write_block(self, block, data, credential):
        from .hf import data_block
        require(self.card and data_block(block, self.card.unit_count) and len(data) == 16
                and block in self.previous and self.supports(self.card.family), 'Read an eligible block first.')
        key, kind = credential
        require(len(key) == 6 and kind in (0x60, 0x61), 'Invalid Classic key.')
        previous = self.previous.pop(block)
        result = await self.response(18, self.uid() + struct.pack('<HB', block, kind) + key + previous + data, 16, data)
        self.previous[block] = result

    def auth(self):
        require(self.authentication == b'\0' or self.supports('ntag_auth'),
                'This Grove reader does not support supplied NTAG passwords.')
        return self.authentication

    async def read_pages(self, page, count=1):
        require(self.card and self.card.ntag and self.supports('ntag21x') and 1 <= count <= 16
                and 0 <= page < page + count <= self.card.unit_count, 'Invalid NTAG page range.')
        result = await self.response(19, self.uid() + struct.pack('<HB', page, count) + self.auth(), count * 4)
        for offset in range(count):
            self.previous[page+offset] = result[offset*4:offset*4+4]
        return result

    async def write_page(self, page, data):
        from .hf import eligible_units
        require(self.card and self.card.ntag and page in eligible_units(self.card.family)
                and len(data) == 4 and page in self.previous, 'Read a writable NTAG user page first.')
        previous = self.previous.pop(page)
        result = await self.response(20, self.uid() + struct.pack('<H', page) + self.auth() + previous + data, 4, data)
        self.previous[page] = result

    async def reset_authentication(self):
        await self.response(21, b'', 0)
        self.previous.clear()

    async def canonical_format(self, snapshot, overwrite=False):
        from .formatting import preflight, snapshot_bytes
        preflight(snapshot, overwrite)
        require(self.authentication == b'\0', 'Formatting does not accept supplied NTAG authentication.')
        require(await self.identify() == snapshot.card, 'Card changed since review.')
        capability, operation = (('ntag_initialize', 23) if snapshot.card.ntag else ('classic1k_format', 22))
        require(self.supports(capability), 'Firmware does not advertise canonical formatting support.')
        result = await self.endpoint.request(operation, self.uid() + bytes([int(overwrite)])
                                             + snapshot_bytes(snapshot), timeout=120)
        if result:
            await self.endpoint.session.close()
            raise CardError('Malformed formatting response; write outcome unverified.')
        self.previous.clear()

    async def close(self):
        self.authentication = b'\0'
        self.card = None
        self.previous.clear()
