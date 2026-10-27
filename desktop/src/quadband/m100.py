"""JRD-4035 / YRM100 UART protocol. SPDX-License-Identifier: GPL-3.0-only

Reference: M5Stack Unit RFID-UHF Common Control Commands, sections 1 and 2.
"""
import asyncio
from dataclasses import dataclass
from .common import CardError, require
from .uhf import selection as validate_epc


@dataclass
class Frame:
    command: int
    data: bytes
    kind: int = 1


def encode(command, data=b'', kind=0):
    require(0 <= command <= 255 and kind in (0, 1, 2) and len(data) <= 1024,
            'Invalid M100 frame type or payload length.')
    packet = bytes([kind, command])+len(data).to_bytes(2, 'big')+data
    return b'\xBB'+packet+bytes([sum(packet) & 255, 0x7E])


class Decoder:
    def __init__(self):
        self.data = b''

    def feed(self, chunk):
        self.data += chunk
        require(len(self.data) <= 65536, 'M100 response buffer overflow; reconnect.')
        frames = []
        while self.data:
            require(self.data[0] == 0xBB, 'Unexpected M100 data. Check reader profile, port and baud.')
            if len(self.data) < 5:
                break
            length = int.from_bytes(self.data[3:5], 'big')
            require(self.data[1] in (1, 2) and length <= 1024, 'Invalid M100 response type or length.')
            size = length+7
            if len(self.data) < size:
                break
            packet = self.data[:size]
            require(packet[-1] == 0x7E and sum(packet[1:-2]) & 255 == packet[-2],
                    'M100 response checksum or terminator failed; reconnect.')
            frames.append(Frame(packet[2], packet[5:-2], packet[1]))
            self.data = self.data[size:]
        return frames


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xFFFF
    return crc ^ 0xFFFF


def error(data):
    code = data[0] if data else 0
    descriptions = {0x09: 'No matching tag for read', 0x10: 'Tag write failed', 0x15: 'No tag found',
                    0x16: 'Incorrect access password', 0xA3: 'Read exceeds tag memory',
                    0xB3: 'Write exceeds tag memory', 0xA4: 'Tag memory locked', 0xB4: 'Tag memory locked',
                    0x17: 'Invalid command parameters', 0x20: 'RF power setting failed'}
    return CardError(f'{descriptions.get(code, "M100 reader rejected command")} (0x{code:02X}).')


def acknowledge(data):
    require(data == b'\0', 'M100 reader did not acknowledge the command.')


def parse_tag(data):
    require(len(data) >= 7, 'Incomplete M100 tag report.')
    length = (data[1] >> 3)*2
    require(2 <= length <= 62 and len(data) == length+5, 'Invalid M100 EPC report length.')
    require(crc16(data[1:-2]) == int.from_bytes(data[-2:], 'big'), 'M100 tag CRC failed.')
    return {'epc': data[3:-2], 'pc': data[1:3], 'antenna': 1, 'frequency': None,
            'signal': data[0]-256 if data[0] >= 128 else data[0]}


def select_payload(epc, pc):
    validate_epc(epc)
    require(len(epc) <= 28, 'M100 exact selection supports EPCs up to 28 bytes; longer EPCs are inventory-only.')
    require(len(pc) == 2 and (pc[0] >> 3)*2 == len(epc), 'Scan this tag again to obtain its current PC word.')
    mask = pc+epc
    return b'\x01\0\0\0\x10'+bytes([len(mask)*8, 0])+mask


def read_payload(bank, word, count, password):
    require(bank in range(4) and 0 <= word <= 0xFFFF and 1 <= count <= 64 and word+count <= 0x10000,
            'M100 reads require 1–64 words within a 16-bit word address range.')
    require(len(password) == 4, 'A four-byte access password is required.')
    return password+bytes([bank])+word.to_bytes(2, 'big')+count.to_bytes(2, 'big')


def write_payload(bank, word, data, password):
    require(bank in (1, 3) and (bank != 1 or word >= 1) and 2 <= len(data) <= 64 and len(data) % 2 == 0,
            'M100 writes support 1–32 words to User memory or EPC after its CRC word.')
    return read_payload(bank, word, len(data)//2, password)+data


def parse_memory(data, epc, pc, size):
    require(len(data) >= 5, 'Incomplete M100 memory response.')
    identity = data[0]
    require(identity == len(epc)+2 and len(data) == 1+identity+size,
            'Invalid M100 memory response length.')
    require(data[1:3] == pc and data[3:1+identity] == epc, 'Reader returned a different tag or changed PC word.')
    return data[1+identity:]


class M100:
    supports_buzzer = False
    power_range = (18, 26)

    def __init__(self, connection, profile='M5Stack JRD-4035'):
        self.connection, self.profile = connection, profile
        self.scanning = False
        self.on_tag = lambda tag: None
        self.seen = {}
        connection.on_frame = self.received

    def received(self, frame):
        if frame.kind == 2:
            require(frame.command == 0x22, 'Unexpected M100 notification.')
            tag = parse_tag(frame.data)
            if tag['epc'] in self.seen or len(self.seen) < 2000:
                self.seen[tag['epc']] = tag['pc']
            self.on_tag(tag)
            if self.connection.pending and self.connection.pending[0] == 0x22 and not self.connection.pending[1].done():
                self.connection.pending[1].set_result(Frame(0x22, b''))
            return True
        if frame.command == 0xFF:
            pending = self.connection.pending
            if frame.data == b'\x15':
                if pending and pending[0] == 0x22 and not pending[1].done():
                    pending[1].set_result(Frame(0x22, b''))
                return True
            if pending and not pending[1].done():
                pending[1].set_exception(error(frame.data))
                return True
            raise error(frame.data)
        return False

    async def command(self, command, data=b'', timeout=3):
        return (await self.connection.exchange(command, data, timeout)).data

    async def connect(self):
        data = await self.command(0x03, b'\0', timeout=.9)
        require(2 <= len(data) <= 129 and data[0] == 0 and all(32 <= c < 127 for c in data[1:]),
                'Invalid M100 hardware identification.')
        hardware = data[1:].decode('ascii')
        require(any(name in hardware.upper() for name in ('M100', 'QM100', 'JRD', 'YRM')),
                'Reader did not identify as an M100-family UHF module.')
        return f'{self.profile} · {hardware}'

    async def select(self, epc):
        require(epc in self.seen, 'Scan and select this tag before accessing its memory.')
        pc = self.seen[epc]
        payload = select_payload(epc, pc)
        acknowledge(await self.command(0x0C, payload))
        require(await self.command(0x0B) == payload, 'M100 exact PC/EPC selection did not match; no write sent.')
        acknowledge(await self.command(0x12, b'\0'))
        return pc

    async def read(self, epc, bank, word, count, password):
        payload = read_payload(bank, word, count, password)
        pc = await self.select(epc)
        return parse_memory(await self.command(0x39, payload), epc, pc, count*2)

    async def write(self, epc, bank, word, data, password, new_epc=None, expected_pc=None):
        payload = write_payload(bank, word, data, password)
        if new_epc is not None:
            select_payload(new_epc, data[:2])
        current = await self.read(epc, 1, 1, 1, password)
        require(current == self.seen[epc] and (expected_pc is None or current == expected_pc),
                'Tag control word changed; scan/read again before writing.')
        try:
            response = await self.command(0x49, payload)
            require(len(response) >= 5, 'Incomplete M100 write result.')
            reported_epc = response[3:-1]
            require(reported_epc in (epc, new_epc), 'Write result identified a different EPC.')
            expected = data[:2] if new_epc is not None and reported_epc == new_epc else current
            parse_memory(response, reported_epc, expected, 1)
            acknowledge(response[-1:])
            if new_epc is not None:
                self.seen[new_epc] = data[:2]
            actual = await self.read(new_epc or epc, bank, word, len(data)//2, password)
            require(actual == data, 'M100 read-back differs from the requested data.')
        except Exception as exc:
            raise CardError(f'Write not verified; tag may be partly changed. Scan/read before retrying. {exc}') from exc
        return actual

    async def change_epc(self, old, new, password):
        validate_epc(new)
        pc = await self.read(old, 1, 1, 1, password)
        new_pc = bytes([(pc[0] & 7) | ((len(new)//2) << 3), pc[1]])
        select_payload(new, new_pc)
        await self.write(old, 1, 1, new_pc+new, password, new, pc)
        return 'New EPC verified: '+new.hex().upper()

    async def inventory(self, stop):
        acknowledge(await self.command(0x12, b'\1'))
        self.scanning = True
        try:
            while not stop.is_set():
                await self.command(0x22, timeout=1.5)
                await asyncio.sleep(.1)
        finally:
            self.scanning = False
            if not self.connection.closed:
                acknowledge(await self.command(0x28))
        return 'M100 scanning stopped.'

    async def buzzer(self, enabled):
        raise CardError('JRD-4035/YRM100 module firmware has no documented buzzer command.')

    async def power(self, value):
        require(self.power_range[0] <= value <= self.power_range[1], 'Module RF power is limited to 18–26 dBm.')
        data = (value*100).to_bytes(2, 'big')
        acknowledge(await self.command(0xB6, data))
        require(await self.command(0xB7) == data, 'M100 power did not match on read-back.')
        return f'RF power verified: {value} dBm. Region settings were retained.'

    async def close(self):
        await self.connection.close()
