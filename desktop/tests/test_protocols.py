"""Protocol regression fixtures shared with the native application's formats."""
import asyncio
import pytest
from quadband import magstripe as msr, chameleon as cham, uhf, ndef
from quadband.common import CardError
from quadband.transports import FramedConnection


def test_standard_density_and_short_test_limits():
    assert msr.validate(msr.SAMPLE) == msr.SAMPLE
    with pytest.raises(CardError):
        msr.validate(msr.SAMPLE, [75]*3)
    assert msr.validate(msr.SHORT, [75]*3) == msr.SHORT
    with pytest.raises(CardError):
        msr.validate(['lowercase', '', ''])
    assert msr.validate(['%A  B?', ';12=3?', '+55?']) == ['A  B', '12=3', '55']


@pytest.mark.parametrize('mapping,raw,expected', [
    (None, '%A?;12?;34?\r\n', ['A', '12', '34']),
    ([0, 1, 2], 'A\t\t34\r\n', ['A', '', '34']),
    ([1], '123=456\r', ['', '123=456', '']),
    ([0, 1], '\x02A\t123\x03\r', ['A', '123', ''])])
def test_keyboard_fields(mapping, raw, expected):
    assert msr.parse_swipe(raw, mapping) == expected


@pytest.mark.parametrize('raw', ['123456', '%A', '%A?%B?', ';12?+34?;56?', ''])
def test_keyboard_rejects_ambiguous_or_multiple_swipes(raw):
    with pytest.raises(CardError):
        msr.parse_swipe(raw)


@pytest.mark.parametrize('size', [1, 63, 64, 126, 2048])
def test_hid_fragment_roundtrip(size):
    data = bytes(i % 256 for i in range(size))
    decoder = msr.HIDDecoder()
    outputs = [decoder.feed(p) for p in msr.hid_packets(data)]
    assert outputs[-1] == data and all(v is None for v in outputs[:-1])


def test_msr_fragmented_read_and_settings_reply():
    reply = b'\x1bs\x1b\1ABC\x1b\2' + b'123=45\x1b\3' + b'987?\x1c\x1b0'
    for split in range(1, len(reply)):
        decoder = msr.StreamDecoder(b'\x1br')
        assert decoder.feed(reply[:split]) is None
        assert decoder.feed(reply[split:]) == reply
    assert msr.read_tracks(reply) == ['ABC', '123=45', '987']
    decoder = msr.StreamDecoder(b'\x1bo\7\5\5')
    assert decoder.feed(b'\x1b0') is None
    assert decoder.feed(b'\7\5\5') == b'\x1b0\7\5\5'


class MSRChannel:
    framed_hid = False
    def __init__(self, bad_read=False):
        self.sent, self.reply, self.closed, self.bad_read = [], [], False, bad_read
    async def send(self, message):
        self.sent.append(message)
        if message[1] == 0x6F:
            response = b'\x1b0\7\5\5'
        elif message[1] == 0x72:
            response = b'\x1bs\x1b\1'+(b'BAD' if self.bad_read else b'TEST')+b'?\x1c\x1b0'
        else:
            response = b'\x1b0'
        self.reply += [response[:1], response[1:]]
    async def receive(self):
        return self.reply.pop(0)
    async def close(self):
        self.closed = True


async def test_msr_write_requires_second_swipe_no_retry_on_mismatch():
    channel = MSRChannel(True)
    with pytest.raises(CardError, match='not verified'):
        await msr.MSRClient(channel).write_verify(['TEST', '', ''], [210, 75, 210], True, lambda _: None)
    assert sum(m[1] == 0x77 for m in channel.sent) == 1
    assert channel.sent[-1] == b'\x1br'


@pytest.mark.parametrize('module,command', [(cham, 1000), (uhf, 0x72)])
def test_framing_fragmentation_checksum(module, command):
    packet = module.encode(command, b'abc')
    for split in range(1, len(packet)):
        d = module.Decoder()
        assert d.feed(packet[:split]) == []
        frames = d.feed(packet[split:])
        assert frames[0].command == command and frames[0].data == b'abc'
    with pytest.raises(CardError):
        module.Decoder().feed(packet[:-1]+bytes([packet[-1] ^ 1]))
    assert len(module.Decoder().feed(packet+packet)) == 2


class QueueChannel:
    def __init__(self):
        self.queue, self.closed, self.sent = asyncio.Queue(), False, []
    async def send(self, data):
        self.sent.append(data)
    async def receive(self):
        return await self.queue.get()
    async def close(self):
        self.closed = True


async def test_timeout_closes_transport_no_late_ack_reuse():
    channel = QueueChannel()
    connection = FramedConnection(channel, cham.Decoder(), cham.encode)
    with pytest.raises(TimeoutError):
        await connection.exchange(1000, timeout=.01)
    assert channel.closed
    with pytest.raises(CardError, match='disconnected'):
        await connection.exchange(1000)
    assert len(channel.sent) == 1


async def test_wrong_command_closes_chameleon():
    channel = QueueChannel()
    connection = FramedConnection(channel, cham.Decoder(), cham.encode)
    channel.queue.put_nowait(cham.encode(999, b'', status=0x68))
    with pytest.raises(CardError, match='Unexpected'):
        await connection.exchange(1000)
    assert channel.closed


def test_uhf_access_layouts():
    assert uhf.encode(0x72).hex() == 'a0030072eb'
    assert uhf.read_payload(2, 4, 2, bytes(4)) == bytes.fromhex('0200000004000200000000')
    assert uhf.write_payload(3, 1, b'\xaa\xbb', bytes(4)) == bytes.fromhex('0000000003000000010001aabb')
    for args in [(0, 0, b'\0\0', bytes(4)), (1, 0, b'\0\0', bytes(4)), (3, 0, b'\0', bytes(4))]:
        with pytest.raises(CardError):
            uhf.write_payload(*args)


class FakeUHF(uhf.UHF):
    def __init__(self):
        self.epc, self.pc, self.commands, self.filter, self.bad = bytes.fromhex('11223344'), b'\x16\xAB', [], b'', False
    async def command(self, command, data=b'', timeout=3):
        self.commands.append((command, data))
        if command == 0x85:
            self.filter = data
            return b'\x10'
        if command == 0x86:
            return self.filter
        if command == 0x81:
            count = int.from_bytes(data[5:7], 'big')
            memory = self.pc if count == 1 else self.pc+self.epc
            if self.bad and any(c == 0x82 for c, _ in self.commands):
                memory = bytes(len(memory))
            section = 4+len(self.epc)+len(memory)
            return bytes([0, 1, section])+self.pc+self.epc+b'\0\0'+memory+len(memory).to_bytes(2, 'big')+b'\1'
        if command == 0x82:
            self.pc, self.epc = data[11:13], data[13:]
            return b'\x10'
        raise AssertionError(command)


@pytest.mark.parametrize('bad', [False, True])
async def test_epc_change_preserves_pc_and_never_retries(bad):
    client = FakeUHF()
    client.bad = bad
    old, new = client.epc, bytes.fromhex('AABBCCDDEEFF')
    if bad:
        with pytest.raises(CardError, match='not verified'):
            await client.change_epc(old, new, bytes(4))
    else:
        await client.change_epc(old, new, bytes(4))
        assert client.pc == b'\x1E\xAB' and client.epc == new
    assert sum(c == 0x82 for c, _ in client.commands) == 1


def test_lf_file_native_compatibility_and_sizes():
    card = cham.LFCard('EM410x', bytes.fromhex('0102030405'))
    assert cham.LFCard.from_file(card.as_file()) == card
    assert 'password' not in str(card.as_file()).lower()
    assert cham.decode_lf(b'\0d'+card.data, 'EM410x') == card
    with pytest.raises(CardError):
        cham.LFCard('HID Prox', bytes(13))
    with pytest.raises(CardError):
        cham.LFCard.from_file({'schema': 2})


class FakeChameleon:
    def __init__(self, mismatch=False):
        self.calls, self.mismatch = [], mismatch
        self.closed = False
    async def exchange(self, command, data=b'', timeout=3):
        self.calls.append((command, data))
        if command == 1001:
            return cham.Frame(command, 0x68, b'')
        if command == 3016:
            return cham.Frame(command, 0x40, b'')
        if command == 3000:
            return cham.Frame(command, 0x40, bytes(5) if self.mismatch else bytes.fromhex('0102030405'))
        raise AssertionError(command)


@pytest.mark.parametrize('mismatch', [False, True])
async def test_t5577_payload_and_verification(mismatch):
    transport = FakeChameleon(mismatch)
    client = cham.Chameleon(transport)
    client.original_mode, client.capabilities = b'\0', {1001, 3000, 3016}
    card = cham.LFCard('EM410x', bytes.fromhex('0102030405'))
    if mismatch:
        with pytest.raises(CardError, match='not verified'):
            await client.write(card)
    else:
        await client.write(card)
    writes = [data for cmd, data in transport.calls if cmd == 3016]
    assert len(writes) == 3
    assert [d[0] for d in writes] == [1, 2, 0]
    assert writes[-1][1:5] == bytes.fromhex('00148040')
    assert all(d[5:] == bytes(6) for d in writes)


VALUES = {
 'Text': {'text': '  Hello 🌎  '}, 'URL / URI': {'url': 'https://midwestgadgets.org'},
 'Custom URL / URI': {'url': 'myapp://open'}, 'Unit.Link': {'url': 'https://unit.link/test'},
 'Search': {'query': 'NFC & cards'}, 'Payment Link': {'url': 'https://example.org/pay'},
 'Social Networks': {'url': 'https://example.org/person'}, 'Video Link': {'url': 'https://example.org/video'},
 'File Link': {'url': 'https://example.org/file.pdf'}, 'Application': {'package': 'com.example.app'},
 'Mail': {'email': 'a@example.org', 'subject': 'Hi & hello'}, 'Contact': {'name': 'Doe; Jane'},
 'Phone': {'phone': '+1 5551234567'}, 'Location': {'latitude': '42.3314', 'longitude': '-83.0458'},
 'SOS': {'name': 'Example'}, 'Bluetooth': {'address': '00:11:22:33:44:55'}, '3D Printing': {},
 'Wi-Fi': {'ssid': ' Test ', 'password': '12345678'}, 'Data': {'payload': '00 AA FF'}}


@pytest.mark.parametrize('kind', list(VALUES))
def test_every_record_type(kind):
    record = ndef.build(kind, VALUES[kind])
    assert ndef.decode(ndef.encode([record])) == [record]
    assert set(VALUES) == set(ndef.FIELDS)


def test_reference_url_and_long_ndef():
    record = ndef.Record.uri('https://midwestgadgets.org')
    assert ndef.encode([record]) == b'\xd1\1\x13U\4midwestgadgets.org'
    records = [record, ndef.Record.text('🌎'), ndef.Record(2, b'application/octet-stream', b'A'*300, b'id')]
    assert ndef.decode(ndef.encode(records)) == records
    for n in range(1, 23):
        with pytest.raises(CardError):
            ndef.decode(ndef.encode([record])[:n])
    with pytest.raises(CardError):
        ndef.decode(ndef.encode([record])+b'\0')


@pytest.mark.parametrize('kind,values', [('Location', {'latitude': 'nan', 'longitude': '0'}),
    ('URL / URI', {'url': 'example.org'}), ('Wi-Fi', {'ssid': 'test', 'password': 'short'}),
    ('Data', {'payload': 'ABC'}), ('Application', {'package': 'bad name'})])
def test_invalid_record_fields(kind, values):
    with pytest.raises(CardError):
        ndef.build(kind, values)
