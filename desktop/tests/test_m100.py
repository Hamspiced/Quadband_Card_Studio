import asyncio
import pytest
from quadband import m100
from quadband.common import CardError
from quadband.service import Service, StopFlag
from quadband.transports import FramedConnection

EPC = bytes.fromhex('30751FEB705C5904E3D50D70')
PC = bytes.fromhex('3400')
REPORT = bytes.fromhex('C9340030751FEB705C5904E3D50D703A76')


def test_vendor_packets_and_every_fragment_boundary():
    assert m100.encode(0x03, b'\0').hex() == 'bb0003000100047e'
    assert m100.encode(0x39, bytes.fromhex('0000ffff0300000002')).hex() == 'bb003900090000ffff0300000002457e'
    packet = bytes.fromhex('BB02220011C9340030751FEB705C5904E3D50D703A76EF7E')
    for split in range(1, len(packet)):
        decoder = m100.Decoder()
        assert decoder.feed(packet[:split]) == []
        tag = m100.parse_tag(decoder.feed(packet[split:])[0].data)
        assert tag['epc'] == EPC and tag['pc'] == PC and tag['signal'] == -55
        assert tag['frequency'] is None
    assert len(m100.Decoder().feed(packet+packet)) == 2
    embedded = m100.encode(0x39, b'\x7E\xBB\x7E', 1)
    assert m100.Decoder().feed(embedded)[0].data == b'\x7E\xBB\x7E'


@pytest.mark.parametrize('packet', [
    b'\xA0', bytes.fromhex('BB00220000'), bytes.fromhex('BB01220401'),
    bytes.fromhex('BB01280001002B7E'), bytes.fromhex('BB01280001002A00'), bytes(65537)])
def test_malformed_frames_fail_closed(packet):
    with pytest.raises(CardError):
        m100.Decoder().feed(packet)


def test_selection_and_memory_limits():
    payload = m100.select_payload(EPC, PC)
    assert payload == bytes.fromhex('01000000107000')+PC+EPC
    for epc, pc in [(EPC, b'\x28\0'), (bytes(30), b'\x78\0'), (b'\1', b'\x08\0')]:
        with pytest.raises(CardError):
            m100.select_payload(epc, pc)
    for word, count in [(0x10000, 1), (0xFFFF, 2), (0, 0), (0, 65)]:
        with pytest.raises(CardError):
            m100.read_payload(3, word, count, bytes(4))
    for bank, word, data in [(0, 0, bytes(2)), (2, 0, bytes(2)), (1, 0, bytes(2)),
                             (3, 0, bytes(66)), (3, 0, b'\0')]:
        with pytest.raises(CardError):
            m100.write_payload(bank, word, data, bytes(4))
    assert m100.read_payload(2, 0x1234, 2, b'\1\2\3\4') == bytes.fromhex('010203040212340002')
    with pytest.raises(CardError, match='CRC'):
        m100.parse_tag(REPORT[:-1]+b'\0')
    for length in range(len(REPORT)):
        with pytest.raises(CardError):
            m100.parse_tag(REPORT[:length])


class ModuleChannel:
    def __init__(self):
        self.queue = asyncio.Queue()
        self.closed, self.sent = False, []
        self.epc, self.pc, self.user = EPC, PC, bytes(128)
        self.filter = b''
        self.bad_filter = self.wrong_tag = self.ignore_write = self.fail_write = self.drop_write = False
        self.interleave = self.no_tags = False
        self.rf = bytes.fromhex('07d0')

    def identity(self):
        return bytes([len(self.epc)+2])+self.pc+self.epc

    async def send(self, message):
        command, data = message[2], message[5:-2]
        self.sent.append((command, data))
        if command == 0x03:
            reply = b'\0M100 V1.00'
        elif command == 0x0C:
            self.filter = data
            reply = b'\0'
        elif command == 0x0B:
            reply = self.filter[:-1]+b'\0' if self.bad_filter else self.filter
        elif command in (0x12, 0x28):
            reply = b'\0'
        elif command == 0x22:
            if self.no_tags:
                await self.queue.put(m100.encode(0xFF, b'\x15', 1))
            else:
                report = m100.encode(0x22, REPORT, 2)
                await self.queue.put(report+report)
            return
        elif command == 0x39:
            bank, word, count = data[4], int.from_bytes(data[5:7], 'big'), int.from_bytes(data[7:9], 'big')
            memory = b'\0\0'+self.pc+self.epc+bytes(128) if bank == 1 else self.user
            reply = self.identity()+memory[word*2:(word+count)*2]
            if self.wrong_tag:
                reply = reply[:3]+bytes([reply[3] ^ 1])+reply[4:]
        elif command == 0x49:
            if self.drop_write:
                return
            if self.fail_write:
                await self.queue.put(m100.encode(0xFF, b'\xB3', 1))
                return
            reply = self.identity()+b'\0'
            if not self.ignore_write:
                if data[4] == 1:
                    self.pc, self.epc = data[9:11], data[11:]
                else:
                    offset = int.from_bytes(data[5:7], 'big')*2
                    self.user = self.user[:offset]+data[9:]+self.user[offset+len(data)-9:]
        elif command == 0xB6:
            self.rf, reply = data, b'\0'
        elif command == 0xB7:
            reply = self.rf
        else:
            raise AssertionError(f'Unexpected command 0x{command:02X}')
        prefix = m100.encode(0x22, REPORT, 2) if self.interleave and command == 0x39 else b''
        packet = prefix+m100.encode(command, reply, 1)
        for chunk in [packet[:3], packet[3:]]:
            await self.queue.put(chunk)

    async def receive(self):
        return await self.queue.get()

    async def close(self):
        self.closed = True


def reader(channel):
    connection = FramedConnection(channel, m100.Decoder(), m100.encode)
    client = m100.M100(connection)
    client.seen[EPC] = PC
    return client


async def test_epc_change_preserves_pc_and_verifies_new_exact_selection():
    channel = ModuleChannel()
    client = reader(channel)
    try:
        assert 'M100 V1.00' in await client.connect()
        await client.change_epc(EPC, b'\1\2\3\4', bytes(4))
        assert channel.pc == b'\x14\0' and channel.epc == b'\1\2\3\4'
        writes = [data for cmd, data in channel.sent if cmd == 0x49]
        assert len(writes) == 1
        assert writes[0] == bytes.fromhex('000000000100010003140001020304')
        assert channel.filter == m100.select_payload(channel.epc, channel.pc)
    finally:
        await client.close()


async def test_user_write_verified_with_interleaved_reports():
    channel = ModuleChannel()
    channel.interleave = True
    client = reader(channel)
    try:
        assert await client.write(EPC, 3, 2, b'\xAA\xBB', bytes(4)) == b'\xAA\xBB'
        assert channel.epc == EPC and channel.pc == PC
        assert sum(cmd == 0x49 for cmd, _ in channel.sent) == 1
    finally:
        await client.close()


@pytest.mark.parametrize('failure', ['bad_filter', 'wrong_tag', 'ignore_write', 'fail_write'])
async def test_failed_preflight_or_write_never_retries(failure):
    channel = ModuleChannel()
    setattr(channel, failure, True)
    client = reader(channel)
    try:
        with pytest.raises(CardError):
            await client.write(EPC, 3, 0, b'\1\2', bytes(4))
        assert sum(cmd == 0x49 for cmd, _ in channel.sent) == (0 if failure in ('bad_filter', 'wrong_tag') else 1)
    finally:
        await client.close()


async def test_write_timeout_closes_connection_and_no_late_reply_reuse():
    channel = ModuleChannel()
    channel.drop_write = True
    client = reader(channel)
    original = client.command
    async def command(cmd, data=b'', timeout=3):
        return await original(cmd, data, .02)
    client.command = command
    with pytest.raises(CardError, match='not verified'):
        await client.write(EPC, 3, 0, b'\1\2', bytes(4))
    assert channel.closed
    with pytest.raises(CardError, match='disconnected'):
        await client.command(0xB7)
    assert sum(cmd == 0x49 for cmd, _ in channel.sent) == 1


async def test_no_tag_inventory_stops_cleanly_and_power_does_not_change_region():
    channel = ModuleChannel()
    channel.no_tags = True
    client, stop = reader(channel), StopFlag()
    try:
        task = asyncio.create_task(client.inventory(stop))
        await asyncio.sleep(.03)
        stop.set()
        await task
        assert not channel.closed and channel.sent[-1][0] == 0x28
        await client.power(26)
        before = len(channel.sent)
        for value in [17, 27]:
            with pytest.raises(CardError):
                await client.power(value)
        with pytest.raises(CardError):
            await client.buzzer(True)
        assert len(channel.sent) == before
        assert not any(cmd in (0x07, 0xAB, 0xAD) for cmd, _ in channel.sent)
    finally:
        await client.close()


@pytest.mark.parametrize('profile', ['M5Stack JRD-4035', 'YRM100'])
async def test_service_selects_m100_without_piswords_commands(monkeypatch, profile):
    channel = ModuleChannel()
    monkeypatch.setattr('quadband.service.transports.SerialChannel', lambda path, baud: channel)
    service = Service(lambda _: None)
    assert profile in await service.connect('uhf', 'USB serial', '/test', 115200, reader_profile=profile)
    assert isinstance(service.uhf, m100.M100)
    assert channel.sent[0] == (3, b'\0')
    await service.close()
    assert channel.closed
