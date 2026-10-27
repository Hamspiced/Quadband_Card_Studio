"""Classic 1K snapshots, safe data writes and NDEF. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from dataclasses import dataclass, field
from datetime import datetime, timezone
from .common import CardError, AuthenticationError, require, hex_bytes
from . import ndef


def data_block(block):
    return isinstance(block, int) and 1 <= block < 64 and block % 4 != 3


DATA_BLOCKS = tuple(b for b in range(64) if data_block(b))
FACTORY = (b'\xff'*6, 0x60)


@dataclass(frozen=True)
class CardInfo:
    uid: bytes
    atr: bytes = b''
    atqa: bytes = b''
    sak: int | None = None

    @property
    def classic(self):
        if self.sak is not None:
            return self.sak == 8 and len(self.atqa) == 2 and self.atqa[0] & 31 == 4 and self.atqa[1] == 0 and len(self.uid) in (4, 7)
        return len(self.atr) == 20 and self.atr[:15] == bytes.fromhex('3B8F8001804F0CA000000306030001')


@dataclass
class Snapshot:
    card: CardInfo
    blocks: dict = field(default_factory=dict)
    errors: dict = field(default_factory=dict)
    keys: dict = field(default_factory=dict)


def profile_blocks(profile):
    try:
        require(profile['format'] == 'quadband.classic1k.data' and profile['version'] == 1, 'Unsupported credentials format.')
        name = profile['name']
        require(isinstance(name, str) and 0 < len(name) <= 100 and all(ord(c) >= 32 for c in name), 'Invalid profile name.')
        datetime.fromisoformat(profile['createdAt'].replace('Z', '+00:00'))
        blocks = profile['blocks']
        require(len(blocks) == 47 and {b['number'] for b in blocks} == set(DATA_BLOCKS),
                'A profile must contain all 47 data blocks exactly once. UID, keys and trailers cannot be included.')
        return {b['number']: hex_bytes(b['hex'], 16) for b in blocks}
    except (KeyError, ValueError, TypeError, AttributeError) as exc:
        raise CardError('Invalid Quadband .qcard file.') from exc


def profile_from_snapshot(snapshot, name='Classic 1K data'):
    require(snapshot.card.classic, 'Scan a Classic 1K card first.')
    value = {'format': 'quadband.classic1k.data', 'version': 1, 'name': name,
             'createdAt': datetime.now(timezone.utc).isoformat(timespec='seconds').replace('+00:00', 'Z'),
             'blocks': [{'number': b, 'hex': snapshot.blocks[b].hex(' ').upper()}
                        for b in DATA_BLOCKS if b in snapshot.blocks]}
    profile_blocks(value)
    return value


def access_code(trailer, index):
    require(len(trailer) == 16 and 0 <= index < 3, 'Incomplete sector permissions.')
    c1, c2, c3 = (trailer[7] >> (4+index)) & 1, (trailer[8] >> index) & 1, (trailer[8] >> (4+index)) & 1
    require(c1 ^ ((trailer[6] >> index) & 1) == 1 and c2 ^ ((trailer[6] >> (4+index)) & 1) == 1
            and c3 ^ ((trailer[7] >> index) & 1) == 1, 'Inconsistent Classic access bits.')
    return c1*4 + c2*2 + c3


def writable(snapshot, block):
    trailer = snapshot.blocks.get(block//4*4+3, b'')
    code = access_code(trailer, block % 4)
    return code == 0 or code in (3, 4, 6) and snapshot.keys.get(block//4, (None, None))[1] == 0x61


def mad_crc(data):
    crc = 0xC7
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1D if crc & 128 else 0)) & 255
    return crc


class ClassicNDEF:
    def __init__(self, snapshot):
        self.card = snapshot.card
        require(self.card.classic and all(b in snapshot.blocks for b in (1, 2, 3)),
                'The card directory could not be read. Try the standard keys or enter the card key.')
        mad, trailer = snapshot.blocks[1]+snapshot.blocks[2], snapshot.blocks[3]
        require(len(mad) == 32 and len(trailer) == 16 and trailer[9] & 128 and trailer[9] & 3 == 1
                and mad_crc(mad[1:]) == mad[0], 'This card is not formatted for NFC records. Use Advanced to inspect data.')
        sectors = [s for s in range(1, 16) if mad[s*2:s*2+2] == b'\x03\xe1']
        require(bool(sectors), 'The directory has no NFC record area.')
        self.blocks = [s*4+i for s in sectors for i in range(3)]
        require(all(len(snapshot.blocks.get(b, b'')) == 16 for b in self.blocks), 'Some NFC data blocks could not be read.')
        self.memory = b''.join(snapshot.blocks[b] for b in self.blocks)
        self.writable = all(writable(snapshot, b) and snapshot.blocks[b//4*4+3][9] & 3 == 0 for b in self.blocks)
        cursor, first, last = 0, None, 0
        while cursor < len(self.memory):
            start, tag = cursor, self.memory[cursor]
            cursor += 1
            if tag == 0:
                continue
            if tag == 254:
                break
            require(cursor < len(self.memory), 'Truncated NFC TLV.')
            length = self.memory[cursor]
            cursor += 1
            if length == 255:
                require(cursor+2 <= len(self.memory), 'Truncated NFC TLV length.')
                length = int.from_bytes(self.memory[cursor:cursor+2], 'big')
                cursor += 2
            require(length <= len(self.memory)-cursor, 'NFC message exceeds card memory.')
            if tag == 3 and first is None:
                first = (start, cursor, cursor+length)
            cursor += length
            last = cursor
        require(first is not None, 'The NFC area has no NDEF message.')
        start, payload, end = first
        self.prefix, self.suffix = self.memory[:start], self.memory[end:max(end, last)]
        self.records = ndef.decode(self.memory[payload:end])
        self.size = end-payload
        self.capacity = max(0, len(self.memory)-len(self.prefix)-len(self.suffix)-(4 if len(self.memory) > 256 else 2))
        self.length_position = len(self.prefix)+1

    def replacing(self, records):
        message = ndef.encode(records)
        require(len(message) <= 65535, 'NFC message too large.')
        length = bytes([len(message)]) if len(message) < 255 else b'\xff'+len(message).to_bytes(2, 'big')
        final = self.prefix + b'\3' + length + message + self.suffix
        require(len(final) <= len(self.memory), f'Records exceed the {self.capacity}-byte message capacity.')
        if len(final) < len(self.memory):
            final += b'\xfe'
        return final.ljust(len(self.memory), b'\0')


class ClassicClient:
    def __init__(self, backend):
        self.backend = backend

    async def same_card(self, expected):
        require(await self.backend.identify() == expected, 'The card changed. Read the target again.')

    async def read(self, supplied=FACTORY, automatic=True):
        require(len(supplied[0]) == 6 and supplied[1] in (0x60, 0x61), 'Invalid six-byte Classic key.')
        card = await self.backend.identify()
        require(card.classic, 'Card identified, but memory operations require MIFARE Classic 1K.')
        snapshot = Snapshot(card)
        for sector in range(16):
            await asyncio.sleep(0)
            standard = (bytes.fromhex('A0A1A2A3A4A5' if sector == 0 else 'D3F7D3F7D3F7'), 0x60)
            candidates = ([standard, FACTORY] if supplied == FACTORY else [supplied, standard, FACTORY]) if automatic else [supplied]
            chosen, error = None, 'No configured key could read this sector.'
            for credential in dict.fromkeys(candidates):
                try:
                    snapshot.blocks[sector*4] = await self.backend.read_block(sector*4, credential)
                    chosen = credential
                    snapshot.keys[sector] = credential
                    break
                except AuthenticationError as exc:
                    error = str(exc)
                    await self.backend.reset_authentication()
                    await self.same_card(card)
            if chosen is None:
                snapshot.errors.update({b: error for b in range(sector*4, sector*4+4)})
                continue
            for block in range(sector*4+1, sector*4+4):
                try:
                    snapshot.blocks[block] = await self.backend.read_block(block, chosen)
                except AuthenticationError as exc:
                    snapshot.errors[block] = str(exc)
        await self.same_card(card)
        return snapshot

    async def update(self, block, data, credential, expected):
        require(data_block(block) and len(data) == 16, 'UID and sector trailers are protected; write exactly 16 data bytes.')
        await self.same_card(expected)
        await self.backend.write_block(block, data, credential)
        require(await self.backend.read_block(block, credential) == data, f'Block {block} did not match on read-back.')
        await self.same_card(expected)

    async def write_block(self, block, data, snapshot):
        require(data_block(block) and len(data) == 16 and block in snapshot.blocks, 'Read a valid data block before writing.')
        require(writable(snapshot, block), 'The block is not writable with the available key.')
        await self.same_card(snapshot.card)
        key = snapshot.keys[block//4]
        require(await self.backend.read_block(block, key) == snapshot.blocks[block], 'Block changed since it was read.')
        try:
            await self.update(block, data, key, snapshot.card)
        except Exception as exc:
            raise CardError(f'Write was attempted but not verified; read the card again. {exc}') from exc

    async def write_profile(self, profile, supplied=FACTORY):
        blocks = profile_blocks(profile)
        target = await self.read(supplied)
        require(not target.errors and len(target.blocks) == 64, 'Read all target sectors before copying; check keys and placement.')
        # Preflight every permission before the first data mutation.
        require(all(target.blocks[b] == value or writable(target, b) for b, value in blocks.items()), 'Some target data blocks are not writable with the available key.')
        verified = 0
        try:
            for b, value in sorted(blocks.items()):
                await asyncio.sleep(0)
                key = target.keys[b//4]
                await self.same_card(target.card)
                require(await self.backend.read_block(b, key) == target.blocks[b], 'Target data changed during copying.')
                if target.blocks[b] != value:
                    await self.update(b, value, key, target.card)
                verified += 1
        except Exception as exc:
            raise CardError(f'Copy incomplete after {verified}/47 blocks. The target may be partly changed. {exc}') from exc
        return target.card

    async def write_records(self, records, expected, supplied=FACTORY):
        target = await self.read(supplied)
        current = ClassicNDEF(target)
        require(current.card == expected.card and current.memory == expected.memory and current.blocks == expected.blocks,
                'Card or NFC records changed; read the target again.')
        require(current.writable, 'NFC area is read-only with the available keys.')
        final = current.replacing(records)
        staging = bytearray(final)
        staging[current.length_position] = 0
        commit = current.length_position//16
        async def update(index, data):
            b = current.blocks[index]
            await self.update(b, bytes(data), target.keys[b//4], target.card)
        try:
            initial = bytearray(current.memory[commit*16:commit*16+16])
            initial[current.length_position % 16] = 0
            await update(commit, initial)
            for i in range(len(current.blocks)):
                await asyncio.sleep(0)
                data = staging[i*16:i*16+16]
                if i == commit or data != current.memory[i*16:i*16+16]:
                    await update(i, data)
            await update(commit, final[commit*16:commit*16+16])
            result = await self.read(supplied)
            require(result.card == target.card and ClassicNDEF(result).records == records, 'Final record verification failed.')
            return result
        except Exception as exc:
            raise CardError(f'Record write not verified; the NFC message may be empty or incomplete. Read the card again. {exc}') from exc


async def copy_forever(client_factory, profile, supplied, stop, progress, result, interval=.4):
    """Require an absent-card observation initially and after EVERY outcome, even identical UIDs.

    A connection failure resets the removal gate; it never masquerades as card removal.
    Factory returns an async context manager so PC/SC handles are released on each poll.
    """
    profile_blocks(profile)
    removed = False
    while not stop.is_set():
        try:
            async with client_factory() as client:
                present = await client.backend.present()
                if not present:
                    removed = True
                    progress('Place the next target card.' if removed else 'Remove the source card.')
                elif removed:
                    removed = False
                    try:
                        card = await client.write_profile(profile, supplied)
                        result(True, f'Verified 47 data blocks · {card.uid.hex().upper()}')
                    except Exception as exc:
                        result(False, str(exc))
                    progress('Remove this card before presenting the next target.')
                else:
                    progress('Remove the card to arm the next copy.')
        except Exception as exc:
            removed = False
            progress(f'Reader unavailable: {exc} Reconnect, then remove and present a card.')
        await asyncio.sleep(interval)
