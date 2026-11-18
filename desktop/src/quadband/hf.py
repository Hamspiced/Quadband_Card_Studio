"""Classic 1K snapshots, safe data writes and NDEF. SPDX-License-Identifier: GPL-3.0-only"""
import asyncio
from dataclasses import dataclass, field
from datetime import datetime, timezone
from .common import CardError, AuthenticationError, require, hex_bytes
from . import ndef


def data_block(block):
    return isinstance(block, int) and 1 <= block < 64 and block % 4 != 3


class NotFormatted(CardError):
    """The card is readable but has no NFC Forum directory."""


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
        if not (len(mad) == 32 and len(trailer) == 16 and trailer[9] & 128 and trailer[9] & 3 == 1 and mad_crc(mad[1:]) == mad[0]):
            raise NotFormatted('This card is not formatted for NFC records. Use Advanced to inspect data.')
        sectors = [s for s in range(1, 16) if mad[s*2:s*2+2] == b'\x03\xe1']
        if not sectors:
            raise NotFormatted('The directory has no NFC record area.')
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
        factory_seen = False
        for sector in range(16):
            await asyncio.sleep(0)
            standard = (bytes.fromhex('A0A1A2A3A4A5' if sector == 0 else 'D3F7D3F7D3F7'), 0x60)
            candidates = ([standard, FACTORY] if supplied == FACTORY else [supplied, standard, FACTORY]) if automatic else [supplied]
            if factory_seen and FACTORY in candidates:
                # A blank card uses factory keys everywhere; skip a failed authentication per sector.
                candidates.remove(FACTORY)
                candidates.insert(0, FACTORY)
            chosen, error = None, 'No configured key could read this sector.'
            for credential in dict.fromkeys(candidates):
                try:
                    snapshot.blocks[sector*4] = await self.backend.read_block(sector*4, credential)
                    chosen = credential
                    snapshot.keys[sector] = credential
                    factory_seen = credential == FACTORY
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

    async def format_ndef(self, snapshot, supplied=FACTORY, erase_data=False):
        """Format a blank factory-key Classic 1K for NFC records (MAD1 plus 15 NDEF sectors) and verify it.

        Only cards whose sectors all still use factory keys and factory access bits are touched. Unless
        erase_data is set, existing data must already be empty.
        """
        backend = self.backend
        require(hasattr(backend, 'write_trailer'), 'Formatting needs a PC/SC reader; this reader can only write cards that are already NFC-formatted.')
        card = snapshot.card
        require(card.classic and not snapshot.errors and len(snapshot.blocks) == 64,
                'The tag could not be fully read, so it was not formatted. It may use unknown keys.')
        require(all(snapshot.keys.get(s) == FACTORY and snapshot.blocks[s*4+3][6:9] == bytes.fromhex('FF0780') for s in range(16)),
                'This tag is locked with non-factory keys, so it was not formatted.')
        data = [b for b in range(1, 64) if data_block(b)]
        require(erase_data or all(not any(snapshot.blocks[b]) for b in data),
                'This tag already holds data. Enable overwriting tags that already hold data to format it.')
        mad = bytearray(b'\0\0' + b'\3\xe1'*15)
        mad[0] = mad_crc(mad[1:])
        trailers = {s*4+3: bytes.fromhex('D3F7D3F7D3F77F078840FFFFFFFFFFFF') for s in range(1, 16)}
        trailers[3] = bytes.fromhex('A0A1A2A3A4A5787788C1FFFFFFFFFFFF')
        order = [b for b in sorted(trailers) if b != 3] + [3]
        plan = {1: bytes(mad[:16]), 2: bytes(mad[16:]), 4: b'\3\0\xfe'.ljust(16, b'\0')}
        plan.update({b: bytes(16) for b in data if b not in plan})
        done = 0
        try:
            for block, value in plan.items():
                await asyncio.sleep(0)
                if snapshot.blocks[block] != value:
                    await self.update(block, value, FACTORY, card)
            for block in order:
                await asyncio.sleep(0)
                await self.same_card(card)
                await backend.write_trailer(block, trailers[block], FACTORY)
                done += 1
                check = await backend.read_block(block, (trailers[block][:6], 0x60))
                require(check[6:10] == trailers[block][6:10], f'Sector {block//4} access settings did not verify.')
            result = await self.read(supplied)
            area = ClassicNDEF(result)
            require(result.card == card and not area.records and area.writable, 'Formatted tag did not verify.')
            return result
        except Exception as exc:
            raise CardError(f'Formatting did not complete ({done}/16 sectors secured); set this tag aside. {exc}') from exc

    async def encode_tag(self, records, supplied=FACTORY, erase_data=False):
        """Format if needed, write the records and verify. Returns (snapshot, formatted, written)."""
        snapshot = await self.read(supplied)
        formatted = False
        try:
            area = ClassicNDEF(snapshot)
        except NotFormatted:
            snapshot = await self.format_ndef(snapshot, supplied, erase_data)
            area, formatted = ClassicNDEF(snapshot), True
        require(area.writable, 'NFC area is read-only with the available keys.')
        if area.records == records:
            return snapshot, formatted, False
        return await self.write_records(records, area, supplied), formatted, True


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


async def encode_forever(client_factory, records, supplied, stop, phase, result, interval=.3, erase_data=False, settle=.15):
    """Encode the same records onto one tag after another until stopped.

    phase(state, text) drives the on-screen prompt; result(success, detail) feeds the tally.
    A tag must leave the reader after every outcome before the next is accepted, a tag whose identity
    changes (moved, swapped, or a second tag entering the field) is never written, and a UID already
    encoded this session is skipped rather than rewritten. Reader trouble and unreadable tags (for
    example several tags on the antenna) are reported without being counted as tag failures.
    """
    ndef.encode(records)
    armed, done, hold, shown = True, set(), ('wait', 'Remove the tag'), None

    def show(state, text):
        nonlocal shown
        if shown != (state, text):
            shown = (state, text)
            phase(state, text)

    while not stop.is_set():
        try:
            async with client_factory() as client:
                backend = client.backend
                if not await backend.present():
                    armed = True
                    show('ready', 'Present a tag')
                elif not armed:
                    show(*hold)
                else:
                    armed = False
                    hold = ('wait', 'Remove the tag')
                    card = await backend.identify()
                    await asyncio.sleep(settle)
                    require(await backend.identify() == card, 'The tag moved or more than one tag is on the reader.')
                    uid = card.uid.hex().upper()
                    if uid in done:
                        hold = ('warn', f'Tag {uid} was already encoded. Remove it and present a new one')
                        show(*hold)
                    else:
                        show('writing', f'Writing {uid}… keep the tag still')
                        try:
                            _, formatted, written = await client.encode_tag(records, supplied, erase_data)
                            require(await backend.identify() == card, 'The tag changed while writing.')
                        except Exception as exc:
                            result(False, f'{uid} · {exc}')
                            hold = ('error', f'Failed — remove tag {uid} and set it aside')
                        else:
                            done.add(uid)
                            result(True, f'{uid} · ' + ('formatted and written' if formatted else 'written' if written else 'already correct') + ', verified')
                            hold = ('done', f'Done — remove tag {uid}')
                        show(*hold)
        except Exception as exc:
            armed = False
            hold = ('warn', f'{exc} If several tags are on the reader, remove them all, then present one tag')
            show(*hold)
        await asyncio.sleep(interval)
