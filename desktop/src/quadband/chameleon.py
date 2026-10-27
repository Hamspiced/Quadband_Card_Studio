"""Chameleon Ultra official 2.x commands. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from dataclasses import dataclass
from .common import CardError, NoCard, AuthenticationError, require
from . import t5577


@dataclass
class Frame:
    command: int
    status: int
    data: bytes


def encode(command, data=b'', status=0):
    require(len(data) <= 4096, 'Chameleon packet too large.')
    header = b'\x11\xef' + command.to_bytes(2, 'big') + status.to_bytes(2, 'big') + len(data).to_bytes(2, 'big')
    message = header + bytes([-sum(header) & 255]) + data
    return message + bytes([-sum(message) & 255])


class Decoder:
    def __init__(self):
        self.data = b''

    def feed(self, chunk):
        self.data += chunk
        out = []
        while self.data:
            require(self.data[0] == 0x11, 'Invalid Chameleon packet start.')
            if len(self.data) < 9:
                break
            require(self.data[1] == 0xEF and sum(self.data[:9]) % 256 == 0, 'Chameleon header checksum failed.')
            length = int.from_bytes(self.data[6:8], 'big')
            require(length <= 4096, 'Chameleon response too large.')
            if len(self.data) < length+10:
                break
            require(sum(self.data[:length+10]) % 256 == 0, 'Chameleon checksum failed.')
            out.append(Frame(int.from_bytes(self.data[2:4], 'big'), int.from_bytes(self.data[4:6], 'big'),
                             self.data[9:9+length]))
            self.data = self.data[length+10:]
        return out


FORMATS = {'EM410x': (3000, 3001, 5), 'EM410x Electra': (3000, 3006, 13),
           'HID Prox': (3002, 3003, 13), 'Viking': (3004, 3005, 4),
           'ioProx': (3010, 3011, 16), 'PAC / Stanley': (3014, 3015, 8),
           'Jablotron': (3019, 3020, 5), 'Pyramid': (3021, None, 16)}


@dataclass(frozen=True)
class LFCard:
    format: str
    data: bytes

    def __post_init__(self):
        require(self.format in FORMATS and len(self.data) == FORMATS[self.format][2], 'Invalid LF format or ID length.')
        if self.format == 'HID Prox':
            require(self.data[0] in [*range(1, 31), 42], 'Unknown HID format; select the format hint and scan again.')
        if self.format == 'PAC / Stanley':
            require(all(32 <= b <= 126 for b in self.data), 'Invalid PAC ID.')

    def matches(self, other):
        return other is not None and self.format == other.format and (
            self.data[:12] == other.data[:12] if self.format == 'ioProx' else self.data == other.data)

    def as_file(self):
        return {'schema': 1, 'kind': 'Quadband LF card', 'card': {'format': self.format, 'bytes': list(self.data)}}

    @classmethod
    def from_file(cls, value):
        try:
            require(value['schema'] == 1 and value['kind'] == 'Quadband LF card', 'Unsupported LF file.')
            return cls(value['card']['format'], bytes(value['card']['bytes']))
        except (KeyError, TypeError, ValueError) as exc:
            raise CardError('Invalid Quadband LF file.') from exc


def decode_lf(data, format):
    if format in ('EM410x', 'EM410x Electra'):
        if len(data) == 5:
            return LFCard('EM410x', data)
        if len(data) == 7 and int.from_bytes(data[:2], 'big') in (100, 101, 102, 103):
            return LFCard('EM410x', data[2:])
        if len(data) == 15 and data[:2] == b'\0h':
            return LFCard('EM410x Electra', data[2:])
        raise CardError('Unknown EM410x response format.')
    return LFCard(format, data[:13] if format == 'HID Prox' and len(data) == 16 else data)


class Chameleon:
    def __init__(self, transport):
        self.transport, self.capabilities, self.original_mode = transport, set(), None

    async def command(self, command, data=b'', status=0x68, timeout=3):
        t5577.guard(command, data)
        reply = await self.transport.exchange(command, data, timeout)
        if reply.status != status:
            if reply.status in (1, 0x41):
                raise NoCard('No card detected. Center one card on the correct antenna.')
            if command in (2008, 2009) and reply.status in (2, 3, 4, 5, 6, 7):
                raise AuthenticationError('Classic authentication or memory access failed. Check key and placement.')
            raise CardError(f'Chameleon status {reply.status:04X}; check firmware capabilities and reader mode.')
        return reply.data

    async def connect(self):
        version = await self.command(1000)
        require(len(version) == 2 and version[0] == 2, 'Compatible official Chameleon 2.x firmware is required.')
        require(await self.command(1033) == b'\0', 'Chameleon Ultra is required; Lite has no reader hardware.')
        capabilities = await self.command(1035)
        require(bool(capabilities) and len(capabilities) % 2 == 0, 'Invalid capability list.')
        self.capabilities = {int.from_bytes(capabilities[i:i+2], 'big') for i in range(0, len(capabilities), 2)}
        require({1001, 1002} <= self.capabilities, 'Reader mode unavailable.')
        mode = await self.command(1002)
        require(mode in (b'\0', b'\1'), 'Invalid device mode.')
        self.original_mode = mode
        revision = (await self.command(1017)).decode('utf-8', 'replace')[:80] if 1017 in self.capabilities else ''
        return f'Chameleon Ultra {version[0]}.{version[1]} · {revision}'

    async def reader_mode(self):
        require(self.original_mode is not None, 'Connect the Chameleon first.')
        await self.command(1001, b'\1')

    async def close(self):
        try:
            if self.original_mode is not None and not self.transport.closed:
                await self.command(1001, self.original_mode)
        finally:
            await self.transport.close()

    async def scan(self, format=None, hint=0):
        await self.reader_mode()
        formats = [format] if format else [f for f in FORMATS if f != 'EM410x Electra' and FORMATS[f][0] in self.capabilities]
        for f in formats:
            require(FORMATS[f][0] in self.capabilities, f'{f} is unavailable in this firmware.')
            try:
                data = await self.command(FORMATS[f][0], bytes([hint if f == 'HID Prox' else 0])
                                          if f in ('HID Prox', 'ioProx') else b'', status=0x40, timeout=5)
                return decode_lf(data, f)
            except NoCard:
                continue
        return None

    async def write(self, card):
        require(3016 in self.capabilities and FORMATS[card.format][0] in self.capabilities,
                'Password-free writing requires firmware with command 3016 and a matching scan command. Legacy password-setting writers are disabled.')
        words = t5577.blocks(card)
        await self.reader_mode()
        try:
            for block, word in words:
                reply = await self.command(3016, t5577.payload(block, word), status=0x40, timeout=5)
                require(not reply, 'Unexpected T5577 block-write response.')
            await asyncio.sleep(.25)
            actual = await self.scan(card.format)
            require(card.matches(actual), 'The target did not read back with the expected ID.')
        except Exception as exc:
            raise CardError(f'Write not verified. The target may be partly changed; scan before retrying. {exc}') from exc
        return 'T5577 identity verified. No password was written; password protection was not enabled.'

    async def identify(self):
        from .hf import CardInfo
        require(2000 in self.capabilities, 'HF scanning is unavailable.')
        await self.reader_mode()
        data = await self.command(2000, status=0)
        require(bool(data) and data[0] in (4, 7, 10) and len(data) >= data[0]+5, 'Invalid HF response.')
        n = data[0]
        require(len(data) == n+5+data[n+4], 'Keep exactly one HF card on the antenna.')
        return CardInfo(data[1:n+1], atqa=data[n+1:n+3], sak=data[n+3])

    async def present(self):
        try:
            await self.identify()
            return True
        except NoCard:
            return False

    async def read_block(self, block, credential):
        require(2008 in self.capabilities and 0 <= block < 64, 'HF block read is unavailable.')
        key, kind = credential
        require(len(key) == 6 and kind in (0x60, 0x61), 'Invalid Classic key.')
        data = await self.command(2008, bytes([kind, block]) + key, status=0)
        require(len(data) == 16, 'Incomplete HF block.')
        return data

    async def write_block(self, block, data, credential):
        from .hf import data_block
        require(2009 in self.capabilities and data_block(block) and len(data) == 16, 'Only Classic data blocks can be written.')
        key, kind = credential
        require(len(key) == 6 and kind in (0x60, 0x61), 'Invalid Classic key.')
        require(not await self.command(2009, bytes([kind, block])+key+data, status=0), 'Unexpected HF write response.')

    async def reset_authentication(self):
        await self.identify()
