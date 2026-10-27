"""Piswords UHF2/UCM60x commands. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from dataclasses import dataclass
from .common import CardError, require


@dataclass
class Frame:
    address: int
    command: int
    data: bytes


def encode(command, data=b'', address=0):
    require(0 <= address < 255 and len(data) <= 252, 'Invalid UHF address or packet length.')
    packet = bytes([0xA0, len(data)+3, address, command])+data
    return packet + bytes([-sum(packet) & 255])


class Decoder:
    def __init__(self):
        self.data = b''

    def feed(self, chunk):
        self.data += chunk
        require(len(self.data) <= 65536, 'UHF response overflow.')
        frames = []
        while self.data:
            require(self.data[0] == 0xA0, 'Unexpected UHF data; check baud rate and port.')
            if len(self.data) < 2:
                break
            size = self.data[1]+2
            require(size >= 5, 'Invalid UHF packet size.')
            if len(self.data) < size:
                break
            require(sum(self.data[:size]) % 256 == 0, 'UHF checksum failed; reconnect.')
            frames.append(Frame(self.data[2], self.data[3], self.data[4:size-1]))
            self.data = self.data[size:]
        return frames


def acknowledge(data):
    errors = {0x36: 'No matching tag', 0x40: 'Access denied', 0x43: 'Memory outside tag capacity',
              0x63: 'Tag memory locked', 0x64: 'Incorrect password', 0x67: 'Wrong reader address'}
    require(data == b'\x10', errors.get(data[0] if data else 0, 'Reader rejected command') + ' · '+data.hex())


def selection(epc):
    require(2 <= len(epc) <= 62 and len(epc) % 2 == 0, 'EPC requires 2–62 bytes in complete words.')
    return bytes([0, len(epc)])+epc


def parse_tag(data):
    require(len(data) >= 12, 'Incomplete UHF report.')
    length = (data[1] >> 3)*2
    require(length > 0 and len(data) == length+10, 'Invalid UHF EPC length.')
    frequency = int.from_bytes(data[-3:], 'big')
    require(1 <= data[0] <= 8 and 840000 <= frequency <= 960000, 'Invalid UHF report antenna/frequency.')
    return {'epc': data[3:3+length], 'pc': data[1:3], 'antenna': data[0], 'frequency': frequency,
            'signal_raw': int.from_bytes(data[3+length:7+length], 'big')}


def read_payload(bank, word, count, password):
    require(bank in range(4) and 0 <= word and 1 <= count <= 64 and word+count <= 0xFFFFFFFF and len(password) == 4,
            'Read 1–64 words with a valid starting word and four-byte password.')
    return bytes([bank])+word.to_bytes(4, 'big')+count.to_bytes(2, 'big')+password


def write_payload(bank, word, data, password):
    require(bank in (1, 3) and (bank != 1 or word >= 1) and 2 <= len(data) <= 128 and len(data) % 2 == 0,
            'Write complete words to User memory or EPC after the CRC word.')
    read_payload(bank, word, len(data)//2, password)
    return password+bytes([bank])+word.to_bytes(4, 'big')+(len(data)//2).to_bytes(2, 'big')+data


def parse_read(data, epc, count):
    require(len(data) >= 10 and data[:2] == b'\0\1', 'UHF read did not identify exactly one matching tag.')
    section, length = data[2], (data[3] >> 3)*2
    require(length > 0 and section == 4+length+count*2 and len(data) in (section+6, section+7), 'Invalid UHF memory response length.')
    require(data[5:5+length] == epc, 'Reader returned a different EPC.')
    require(int.from_bytes(data[section+3:section+5], 'big') == count*2, 'Reader returned fewer memory bytes than requested.')
    return data[7+length:3+section]


def check_write(data, old, new=None):
    if len(data) <= 2:
        acknowledge(data)
        return
    require(len(data) >= 10 and data[:2] == b'\0\1', 'Write did not identify exactly one tag.')
    length, section = (data[3] >> 3)*2, data[2]
    require(length > 0 and section == length+4 and len(data) in (length+9, length+10), 'Invalid UHF write response.')
    require(data[5:5+length] in (old, new), 'Write response identified another EPC.')
    acknowledge(data[7+length:8+length])


class UHF:
    def __init__(self, connection, address=0):
        self.connection, self.address = connection, address
        self.scanning = False
        self.on_tag = lambda tag: None
        connection.on_frame = self.received

    def received(self, frame):
        if frame.address != self.address:
            return True
        if frame.command == 0x89:
            if len(frame.data) > 2:
                self.on_tag(parse_tag(frame.data))
                return True
            if self.connection.pending is None or self.connection.pending[0] != 0x89:
                if frame.data != b'\x36':
                    acknowledge(frame.data)
                return True
        return False

    async def command(self, command, data=b'', timeout=3):
        return (await self.connection.exchange(command, data, timeout)).data

    async def connect(self):
        data = await self.command(0x72, timeout=.9)
        require(len(data) == 3 and data[2] in (1, 2, 6, 8, 10), 'Not a compatible Piswords UHF2 reader.')
        return f'UCM60{data[2]} · firmware {data[0]}.{data[1]}'

    async def select(self, epc):
        payload = selection(epc)
        acknowledge(await self.command(0x85, payload))
        require(await self.command(0x86) == payload, 'Reader did not confirm the exact EPC filter. No write sent.')

    async def read(self, epc, bank, word, count, password):
        payload = read_payload(bank, word, count, password)
        await self.select(epc)
        return parse_read(await self.command(0x81, payload), epc, count)

    async def write(self, epc, bank, word, data, password, new_epc=None, expected_pc=None):
        payload = write_payload(bank, word, data, password)
        current = await self.read(epc, 1, 1, 1, password)
        require(expected_pc is None or current == expected_pc, 'Tag control word changed; read again before editing EPC.')
        try:
            check_write(await self.command(0x82, payload), epc, new_epc)
            actual = await self.read(new_epc or epc, bank, word, len(data)//2, password)
            require(actual == data, 'UHF memory did not match on read-back.')
        except Exception as exc:
            raise CardError(f'Write not verified; tag may be partly changed. Scan/read before retrying. {exc}') from exc
        return actual

    async def change_epc(self, old, new, password):
        selection(new)
        pc = await self.read(old, 1, 1, 1, password)
        data = bytes([(pc[0] & 7) | ((len(new)//2) << 3), pc[1]])+new
        await self.write(old, 1, 1, data, password, new, pc)
        return 'New EPC verified: '+new.hex().upper()

    async def inventory(self, stop):
        acknowledge(await self.command(0x85, b'\1'))
        require(await self.command(0x86) == b'\0', 'EPC filter could not be cleared.')
        try:
            self.scanning = True
            acknowledge(await self.command(0x89, b'\1'))
            while not stop.is_set():
                require(not self.connection.closed, 'UHF connection lost.')
                await asyncio.sleep(.15)
        finally:
            if not self.connection.closed:
                acknowledge(await self.command(0x8C))
            self.scanning = False
        return 'UHF scanning stopped.'

    async def buzzer(self, enabled):
        acknowledge(await self.command(0x7A, bytes([2 if enabled else 0])))
        return 'Buzzer command acknowledged.'

    async def power(self, value):
        require(0 <= value <= 33, 'RF power must be 0–33 dBm within the reader’s supported range.')
        acknowledge(await self.command(0x76, bytes([value])))
        actual = await self.command(0x77)
        require(actual and all(b == value for b in actual), 'RF power did not match on read-back.')
        return f'RF power verified: {value} dBm. Region settings were retained.'

    async def close(self):
        if self.scanning and not self.connection.closed:
            try:
                await self.command(0x8C)
            except Exception:
                pass
        await self.connection.close()
