"""NFC Forum Type 2 tags (NXP NTAG213/215/216): NDEF formatting, records and verification. SPDX-License-Identifier: GPL-3.0-only

Layout (NFC Forum Type 2 Tag Operation / NXP NTAG21x data sheet):
  pages 0-1  UID / manufacturer data (never written)
  page 2     internal byte, static lock bytes (permanent; never written)
  page 3     capability container (CC): E1 <version 1.0> <data area size / 8> <access>; one-time programmable
  page 4...  NDEF data area made of TLVs: optional Lock/Memory Control TLVs, the NDEF Message TLV (03), terminator (FE)
  after the data area: dynamic lock bytes, configuration (AUTH0 password start page, ACCESS), PWD, PACK
"""
import asyncio
from dataclasses import dataclass, field
from .common import CardError, NoCard, require
from .hf import CLASSIC_1K, CardInfo, NotFormatted
from . import ndef


class NotNTAG(CardError):
    """The tag on the reader is not an NTAG213, NTAG215 or NTAG216."""


@dataclass(frozen=True)
class Spec:
    name: str
    version: int       # storage-size byte of GET_VERSION
    cc_size: int       # NDEF data area in 8-byte units, as recommended by NXP for the capability container
    dynamic_lock: int  # first page after user memory: dynamic lock bytes, then CFG0 and CFG1
    reserved: int = 0  # bytes NXP pre-programs ahead of the NDEF TLV (NTAG213 Lock Control TLV), kept intact

    @property
    def area(self):
        return self.cc_size*8

    @property
    def end(self):
        return 4+self.area//4

    @property
    def user(self):
        return (self.dynamic_lock-4)*4

    @property
    def capacity(self):
        """Largest NDEF message the data area can hold (short or three-byte TLV length)."""
        room = self.area-self.reserved
        return max(min(254, room-2), room-4)

    @property
    def label(self):
        return f'{self.name} · {self.user} bytes'


TAGS = {s.name: s for s in (Spec('NTAG213', 0x0F, 0x12, 40, 5), Spec('NTAG215', 0x11, 0x3E, 130), Spec('NTAG216', 0x13, 0x6D, 226))}
BY_VERSION = {s.version: s for s in TAGS.values()}
CLASSIC_CAPACITY = 716


def tag_types():
    """Dropdown entries: label -> tag type."""
    return {CLASSIC_1K: CLASSIC_1K, **{s.label: s.name for s in TAGS.values()}}


def capacity(tag):
    require(tag == CLASSIC_1K or tag in TAGS, 'Choose a supported tag type.')
    return CLASSIC_CAPACITY if tag == CLASSIC_1K else TAGS[tag].capacity


def check_fit(tag, records):
    size = len(ndef.encode(records))
    require(size <= capacity(tag), f'These records need {size} bytes; {tag} holds {capacity(tag)}.')
    return size


def spec_from_version(data):
    if not (len(data) == 8 and data[:3] == b'\0\4\4'):
        raise NotNTAG('This is not an NXP NTAG tag.')
    spec = BY_VERSION.get(data[6])
    if spec is None:
        raise NotNTAG(f'This NXP tag (storage code {data[6]:02X}) is not an NTAG213, NTAG215 or NTAG216.')
    return spec


@dataclass
class Type2Snapshot:
    card: CardInfo
    spec: Spec
    verified: bool      # identity confirmed with GET_VERSION
    head: bytes         # pages 0-3
    memory: bytes       # NDEF data area
    tail: bytes         # dynamic lock page, CFG0, CFG1, PWD (pages dynamic_lock ..+3)
    blocks: dict = field(default_factory=dict)
    errors: dict = field(default_factory=dict)

    @property
    def cc(self):
        return self.head[12:16]


def write_problem(snapshot):
    """Why the NDEF area cannot be written with no password, or None."""
    spec, head, tail = snapshot.spec, snapshot.head, snapshot.tail
    if head[10] & 0xF0 or head[11]:
        return 'Some NDEF pages are permanently locked (static lock bytes).'
    if any(tail[:3]):
        return 'Some NDEF pages are permanently locked (dynamic lock bytes).'
    if tail[7] < spec.end:
        return f'The tag is password protected from page {tail[7]}; password-protected tags are not supported.'
    return None


def leading_controls(memory):
    """End of the last Lock Control / Memory Control TLV that precedes any NDEF or other data (0 if none)."""
    i, end = 0, 0
    while i < len(memory):
        if memory[i] == 0:
            i += 1
        elif memory[i] in (1, 2) and i+1 < len(memory) and i+2+memory[i+1] <= len(memory):
            i += 2+memory[i+1]
            end = i
        else:
            break
    return end


def blank(memory):
    """True when the data area holds no user data: only control TLVs, an empty NDEF message, a terminator."""
    rest = memory[leading_controls(memory):]
    if rest[:2] == b'\3\0':
        rest = rest[2:]
    if rest[:1] == b'\xfe':
        rest = rest[1:]
    return not any(rest)


class Type2NDEF:
    def __init__(self, snapshot):
        self.card, self.spec, self.memory = snapshot.card, snapshot.spec, snapshot.memory
        cc = snapshot.cc
        if cc[0] != 0xE1 or cc[1] >> 4 != 1:
            raise NotFormatted('This tag is not formatted for NFC records.')
        if cc[2] != self.spec.cc_size:
            raise NotFormatted(f'The tag declares a {cc[2]*8}-byte NFC area; {self.spec.name} uses {self.spec.area} bytes.')
        self.cc = cc
        self.problem = write_problem(snapshot) or ('The tag is marked read-only.' if cc[3] & 15 else None)
        self.writable = self.problem is None
        try:
            self.parse()
        except NotFormatted:
            raise
        except CardError as exc:
            raise NotFormatted(str(exc)) from exc

    def parse(self):
        memory = self.memory
        cursor, first, last = 0, None, 0
        while cursor < len(memory):
            start, tag = cursor, memory[cursor]
            cursor += 1
            if tag == 0:
                continue
            if tag == 254:
                break
            require(cursor < len(memory), 'Truncated NFC TLV.')
            length = memory[cursor]
            cursor += 1
            if length == 255:
                require(cursor+2 <= len(memory), 'Truncated NFC TLV length.')
                length = int.from_bytes(memory[cursor:cursor+2], 'big')
                cursor += 2
            require(length <= len(memory)-cursor, 'NFC message exceeds tag memory.')
            if tag == 3 and first is None:
                first = (start, cursor, cursor+length)
            cursor += length
            last = cursor
        if first is None:
            raise NotFormatted('The NFC area has no NDEF message.')
        start, payload, end = first
        self.prefix, self.suffix = memory[:start], memory[end:max(end, last)]
        self.records = ndef.decode(memory[payload:end]) if end > payload else []
        self.size = end-payload
        available = len(memory)-len(self.prefix)-len(self.suffix)
        self.capacity = max(0, max(min(254, available-2), available-4))

    def replacing(self, records):
        """Return (final image, staging image, header length) for the data area.

        The staging image carries the new message behind an NDEF TLV of length 0, so an interrupted
        write leaves a valid empty tag; writing the final header is the commit.
        """
        message = ndef.encode(records)
        require(len(message) <= 65534, 'NFC message too large.')
        length = bytes([len(message)]) if len(message) < 255 else b'\xff'+len(message).to_bytes(2, 'big')
        header = self.prefix + b'\3' + length
        body = message + self.suffix
        require(len(header)+len(body) <= len(self.memory), f'Records need {len(message)} bytes; {self.spec.name} holds {self.capacity}.')

        def image(lead):
            data = lead + body
            if len(data) < len(self.memory):
                data += b'\xfe'
            return data.ljust(len(self.memory), b'\0')
        staged = (self.prefix + b'\3\0').ljust(len(header), b'\0')
        return image(header), image(staged), len(header)


class Type2Client:
    def __init__(self, backend):
        self.backend = backend

    async def same_card(self, expected):
        require(await self.backend.identify() == expected, 'The tag changed. Present it again.')

    async def pages(self, start):
        data = await self.backend.read_pages(start)
        require(len(data) == 16, 'Incomplete tag response.')
        return data

    async def read(self, tag=None):
        backend = self.backend
        require(hasattr(backend, 'read_pages') and hasattr(backend, 'write_page'),
                'NTAG tags need a PC/SC reader such as the ACR122U or ACR1552U; the Chameleon cannot encode them here.')
        card = await backend.identify()
        if card.classic or len(card.uid) != 7:
            raise NotNTAG('This is not an NTAG tag. Present an NTAG213, NTAG215 or NTAG216.')
        spec, verified = None, False
        version = await backend.get_version() if hasattr(backend, 'get_version') else None
        if version is not None:
            spec, verified = spec_from_version(version), True
            require(tag is None or spec.name == tag, f'This is an {spec.name}, but {tag} is selected. Choose the matching tag type.')
        elif tag is not None:
            require(tag in TAGS, 'Choose an NTAG213, NTAG215 or NTAG216.')
            spec = TAGS[tag]
        head = await self.pages(0)
        if spec is None:
            spec = next((s for s in TAGS.values() if s.cc_size == head[14] and head[12] == 0xE1), None)
            if spec is None:
                raise NotNTAG('This tag could not be identified as an NTAG213, NTAG215 or NTAG216.')
        memory = b''
        for page in range(4, spec.end, 4):
            await asyncio.sleep(0)
            memory += await self.pages(page)
        try:
            tail = await self.pages(spec.dynamic_lock)
        except NoCard:
            raise
        except CardError as exc:
            raise CardError(f'The {spec.name} configuration could not be read ({exc}). The tag is password protected or is not an {spec.name}.') from exc
        await self.same_card(card)
        return Type2Snapshot(card, spec, verified, head, memory[:spec.area], tail)

    async def put(self, card, spec, pages):
        for page, data in sorted(pages.items()):
            require(len(data) == 4 and (page == 3 or 4 <= page < spec.end),
                    'Only the capability container and NDEF data pages are ever written.')
            await asyncio.sleep(0)
            await self.same_card(card)
            await self.backend.write_page(page, bytes(data))

    async def format_ndef(self, snapshot, erase_data=False):
        """Make a tag NFC-ready: capability container, then an empty NDEF message. Verified by reading back."""
        spec, card, cc = snapshot.spec, snapshot.card, snapshot.cc
        target = bytes([0xE1, 0x10, spec.cc_size, 0])
        require(cc == target or not snapshot.head[10] & 8, 'The capability container is locked with a different value, so this tag cannot be formatted.')
        require(all(c & ~t & 255 == 0 for c, t in zip(cc, target)),
                f'The capability container is permanently set (one-time programmable) and cannot be changed to {spec.name}.')
        problem = write_problem(snapshot)
        require(problem is None, 'This tag cannot be formatted. '+str(problem))
        empty = blank(snapshot.memory)
        require(erase_data or empty, 'This tag already holds data. Enable overwriting tags that already hold data to format it.')
        image = ((snapshot.memory[:leading_controls(snapshot.memory)] if empty else b'') + b'\3\0\xfe').ljust(spec.area, b'\0')
        try:
            if cc != target:
                await self.put(card, spec, {3: target})
                require((await self.pages(0))[12:16] == target, 'The capability container did not verify.')
            current = snapshot.memory
            pages = {4+i: image[i*4:i*4+4] for i in range(spec.area//4) if image[i*4:i*4+4] != current[i*4:i*4+4]}
            first = pages.pop(4, None)
            await self.put(card, spec, pages)
            if first is not None:
                await self.put(card, spec, {4: first})
            result = await self.read(spec.name)
            area = Type2NDEF(result)
            require(result.card == card and not area.records and area.writable, 'Formatted tag did not verify.')
            return result
        except Exception as exc:
            raise CardError(f'Formatting did not complete; set this tag aside. {exc}') from exc

    async def write_records(self, snapshot, area, records):
        spec, card = snapshot.spec, snapshot.card
        require(area.writable, area.problem)
        final, staging, header = area.replacing(records)
        current, count = area.memory, spec.area//4
        def page(image, i):
            return image[i*4:i*4+4]
        head = range((header+3)//4)
        stage_one = {4+i: page(staging, i) for i in head if page(staging, i) != page(current, i)}
        stage_two = {4+i: page(staging, i) for i in range(count) if i not in head and page(staging, i) != page(current, i)}
        commit = {4+i: page(final, i) for i in head if page(final, i) != page(staging, i)}
        try:
            await self.same_card(card)
            await self.put(card, spec, stage_one)
            await self.put(card, spec, stage_two)
            await self.put(card, spec, commit)
            result = await self.read(spec.name)
            require(result.card == card and result.cc == snapshot.cc and Type2NDEF(result).records == records,
                    'Final record verification failed.')
            return result
        except Exception as exc:
            raise CardError(f'Record write not verified; the NFC message may be empty or incomplete. Present the tag and read it again. {exc}') from exc

    async def encode_tag(self, tag, records, erase_data=False):
        """Format if needed, write the records and verify. Returns (snapshot, formatted, written)."""
        check_fit(tag, records)
        snapshot = await self.read(tag)
        formatted = False
        try:
            area = Type2NDEF(snapshot)
        except NotFormatted:
            snapshot = await self.format_ndef(snapshot, erase_data)
            area, formatted = Type2NDEF(snapshot), True
        require(area.writable, area.problem)
        if area.records == records:
            return snapshot, formatted, False
        return await self.write_records(snapshot, area, records), formatted, True
