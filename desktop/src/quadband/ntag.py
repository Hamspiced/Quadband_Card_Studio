"""Guarded NTAG21x data workflows. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from .common import CardError, require
from .hf import Snapshot, ClassicNDEF, eligible_units, profile_blocks


def writable(snapshot, page, authenticated=False):
    card, pages = snapshot.card, snapshot.blocks
    require(card.ntag and page in eligible_units(card.family), 'Protected NTAG page.')
    dynamic = card.unit_count - 5
    require(all(len(pages.get(p, b'')) == 4 for p in (2, 3, dynamic, dynamic+1, dynamic+2)),
            'Read CC, locks and configuration before writing.')
    cc, static, locks = pages[3], pages[2], pages[dynamic]
    require(cc[0] == 0xe1 and cc[1] == 0x10 and 0 < cc[2] * 8 <= len(eligible_units(card.family)) * 4 and cc[3] == 0,
            'Unsupported or read-only NTAG capability container.')
    if page < 8 and static[2] & (1 << page):
        return False
    if 8 <= page < 16 and static[3] & (1 << (page-8)):
        return False
    if page >= 16:
        group = (page-16) // {'ntag213': 2, 'ntag215': 16, 'ntag216': 16}[card.family]
        if int.from_bytes(locks[:2], 'little') & (1 << group):
            return False
    auth0 = pages[dynamic+1][3]
    if page >= auth0 and not authenticated:
        return False
    # Configuration and lock bytes are never treated as user data.
    return True


class Type2NDEF(ClassicNDEF):
    def control_tlv(self, tag, value):
        if len(value) != 3:
            return False
        page_size = 1 << (value[2] & 15)
        offset = value[0] & 15
        address = (value[0] >> 4) * page_size + offset
        if offset >= page_size:
            return False
        size = value[1] or 256
        if tag == 1:
            bits, bytes_locked = {'ntag213': (12, 8), 'ntag215': (8, 64),
                                  'ntag216': (14, 64)}[self.card.family]
            return (address == (self.card.unit_count - 5) * 4 and size == bits
                    and 1 << (value[2] >> 4) == bytes_locked)
        return (value[2] >> 4 == 0 and address >= 16 + len(self.memory)
                and address + size <= self.card.unit_count * 4)
    def __init__(self, snapshot, authenticated=False):
        self.card = snapshot.card
        require(self.card.ntag, 'Not an NTAG21x card.')
        cc = snapshot.blocks.get(3, b'')
        require(len(cc) == 4 and cc[0] == 0xe1 and cc[1] == 0x10
                and 0 < cc[2] * 8 <= len(eligible_units(self.card.family)) * 4
                and cc[3] >> 4 == 0, 'Unsupported NTAG capability container.')
        units = eligible_units(self.card.family)
        size = cc[2] * 8
        self.blocks = list(units[:(size+3)//4])
        require(all(len(snapshot.blocks.get(p, b'')) == 4 for p in self.blocks), 'Incomplete NTAG user memory.')
        self.memory = b''.join(snapshot.blocks[p] for p in self.blocks)[:size]
        try:
            self.writable = all(writable(snapshot, p, authenticated) for p in self.blocks)
        except CardError:
            self.writable = False
        if self.memory and not any(self.memory) and all(snapshot.blocks.get(p) == bytes(4) for p in units):
            self.prefix, self.suffix, self.records = b'', b'', []
            self.size, self.length_position = 0, 1
            self.capacity = len(self.memory) - (4 if len(self.memory) > 256 else 2)
        else:
            self.parse_memory()


class Client:
    def __init__(self, backend):
        self.backend = backend

    @property
    def authenticated(self):
        return getattr(self.backend, 'authentication', b'\0') != b'\0'

    async def same_card(self, expected):
        require(await self.backend.identify() == expected, 'The card changed. Read the target again.')

    async def read(self, supplied=None):
        card = await self.backend.identify()
        require(card.ntag, 'NTAG213/215/216 required.')
        from .formatting import VERSIONS
        require(card.version == VERSIONS[card.family], 'Exact NTAG GET_VERSION identity is required.')
        snapshot = Snapshot(card)
        # Never expose masked password/PACK bytes as known credentials.
        for start in range(0, card.unit_count - 2, 16):
            count = min(16, card.unit_count - 2 - start)
            data = await self.backend.read_pages(start, count)
            require(len(data) == count * 4, 'Incomplete NTAG memory response.')
            for i in range(count):
                snapshot.blocks[start+i] = data[i*4:i*4+4]
            await asyncio.sleep(0)
        await self.same_card(card)
        return snapshot

    async def update(self, page, value, target):
        require(len(value) == 4 and writable(target, page, self.authenticated), 'NTAG page is locked or protected.')
        await self.same_card(target.card)
        dynamic = target.card.unit_count-5
        metadata = await self.backend.read_pages(2, 2)
        tail = await self.backend.read_pages(dynamic, 3)
        require(metadata == target.blocks[2]+target.blocks[3]
                and tail == b''.join(target.blocks[p] for p in range(dynamic, dynamic+3)),
                'NTAG permissions changed since preview.')
        require(await self.backend.read_pages(page) == target.blocks[page], 'NTAG page changed since preview.')
        require(not getattr(self.backend, 'cancelled', lambda: False)(), 'Record write cancelled.')
        await self.backend.write_page(page, value)
        require(await self.backend.read_pages(page) == value, 'NTAG write did not verify.')
        await self.same_card(target.card)
        target.blocks[page] = value

    async def write_block(self, page, value, snapshot):
        current = await self.read()
        require(current.card == snapshot.card and current.blocks == snapshot.blocks, 'NTAG changed since preview.')
        try:
            await self.update(page, value, current)
        except Exception as exc:
            raise CardError(f'NTAG write not verified; read again before retrying. {exc}') from exc

    async def write_profile(self, profile, supplied=None):
        current = await self.read()
        units = profile_blocks(profile, current.card.family)
        require(all(writable(current, p, self.authenticated) for p in units), 'Some NTAG user pages are protected.')
        verified = 0
        try:
            for page, value in sorted(units.items()):
                if current.blocks[page] != value:
                    await self.update(page, value, current)
                verified += 1
            return current.card
        except Exception as exc:
            raise CardError(f'Copy incomplete after {verified}/{len(units)} pages. Target may be partly changed. {exc}') from exc

    async def write_records(self, records, expected, supplied=None):
        current = await self.read()
        area = Type2NDEF(current, self.authenticated)
        require(area.card == expected.card and area.memory == expected.memory and area.blocks == expected.blocks,
                'Card or records changed; read the target again.')
        require(area.writable, 'NTAG NFC area is protected or has an unsupported reserved layout.')
        final = area.replacing(records)
        staging = bytearray(final)
        staging[area.length_position] = 0
        index, offset = divmod(area.length_position, 4)
        page = area.blocks[index]
        initial = bytearray(current.blocks[page])
        initial[offset] = 0
        try:
            await self.update(page, bytes(initial), current)
            for i, p in enumerate(area.blocks):
                value = bytes(staging[i*4:i*4+4])
                if current.blocks[p] != value:
                    await self.update(p, value, current)
            await self.update(page, final[index*4:index*4+4], current)
            result = await self.read()
            require(Type2NDEF(result, self.authenticated).records == records, 'Final NDEF verification failed.')
            return result
        except Exception as exc:
            raise CardError(f'NTAG record write not verified; message may be empty or incomplete. {exc}') from exc
